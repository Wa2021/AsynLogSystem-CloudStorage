#include "ServerBackupLog.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace
{
    class ConnectionGuard
    {
    public:
        ConnectionGuard(int socket_fd, std::atomic<size_t> *active_connections)
            : socket_fd_(socket_fd), active_connections_(active_connections)
        {
        }

        ~ConnectionGuard()
        {
            if (socket_fd_ >= 0)
                close(socket_fd_);
            active_connections_->fetch_sub(1, std::memory_order_relaxed);
        }

    private:
        int socket_fd_;
        std::atomic<size_t> *active_connections_;
    };

    class BackupFileWriter
    {
    public:
        explicit BackupFileWriter(std::string filename,
                                  size_t max_size = 100 * 1024 * 1024,
                                  size_t retained_files = 5)
            : filename_(std::move(filename)), max_size_(max_size),
              retained_files_(retained_files)
        {
        }

        bool Write(const std::string &message)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!RotateIfNeeded(message.size()))
                return false;

            FILE *file = fopen(filename_.c_str(), "ab");
            if (file == nullptr)
                return false;

            size_t written = 0;
            while (written < message.size())
            {
                size_t count = fwrite(message.data() + written, 1,
                                      message.size() - written, file);
                if (count == 0)
                {
                    fclose(file);
                    return false;
                }
                written += count;
            }

            bool ok = true;
            if (message.empty() || message.back() != '\n')
                ok = fputc('\n', file) != EOF;
            if (fflush(file) == EOF || fsync(fileno(file)) == -1)
                ok = false;
            if (fclose(file) == EOF)
                ok = false;
            return ok;
        }

    private:
        bool RotateIfNeeded(size_t incoming_size)
        {
            std::error_code error;
            uintmax_t current_size = std::filesystem::exists(filename_, error)
                                        ? std::filesystem::file_size(filename_, error)
                                        : 0;
            if (error)
                return false;
            if (current_size + incoming_size <= max_size_)
                return true;

            if (retained_files_ == 0)
            {
                std::filesystem::remove(filename_, error);
                return !error;
            }

            std::filesystem::remove(
                filename_ + "." + std::to_string(retained_files_), error);
            error.clear();
            for (size_t index = retained_files_; index > 1; --index)
            {
                std::string from = filename_ + "." + std::to_string(index - 1);
                std::string to = filename_ + "." + std::to_string(index);
                if (std::filesystem::exists(from, error))
                    std::filesystem::rename(from, to, error);
                if (error)
                    return false;
            }
            if (std::filesystem::exists(filename_, error))
                std::filesystem::rename(filename_, filename_ + ".1", error);
            return !error;
        }

    private:
        std::string filename_;
        size_t max_size_;
        size_t retained_files_;
        std::mutex mutex_;
    };
} // namespace

TcpServer::TcpServer(uint16_t port, std::string listen_address,
                     MessageCallback callback, std::string expected_token,
                     size_t max_connections, int receive_timeout_seconds)
    : port_(port), listen_address_(std::move(listen_address)),
      callback_(std::move(callback)), expected_token_(std::move(expected_token)),
      max_connections_(max_connections),
      receive_timeout_seconds_(receive_timeout_seconds)
{
    if (!callback_ || max_connections_ == 0 || receive_timeout_seconds_ <= 0)
        throw std::invalid_argument("invalid backup server configuration");
}

TcpServer::~TcpServer()
{
    if (listen_socket_ >= 0)
        close(listen_socket_);
}

bool TcpServer::Init()
{
    listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ == -1)
        return false;

    int reuse = 1;
    if (setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) == -1)
        return false;

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port_);
    if (inet_pton(AF_INET, listen_address_.c_str(), &local.sin_addr) != 1)
        return false;
    if (bind(listen_socket_, reinterpret_cast<sockaddr *>(&local),
             sizeof(local)) == -1)
        return false;
    return listen(listen_socket_, kBacklog) == 0;
}

void TcpServer::Run()
{
    for (;;)
    {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        int connection = accept(listen_socket_,
                                reinterpret_cast<sockaddr *>(&client_address),
                                &address_length);
        if (connection < 0)
        {
            if (errno == EINTR)
                continue;
            std::cerr << "accept backup connection failed: " << strerror(errno)
                      << std::endl;
            continue;
        }

        size_t previous_connections =
            active_connections_.fetch_add(1, std::memory_order_relaxed);
        if (previous_connections >= max_connections_)
        {
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            close(connection);
            continue;
        }

        char address_buffer[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &client_address.sin_addr, address_buffer,
                      sizeof(address_buffer)) == nullptr)
        {
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            close(connection);
            continue;
        }
        std::string client_info = std::string(address_buffer) + ":" +
                                  std::to_string(ntohs(client_address.sin_port));

        try
        {
            std::thread([this, connection, client_info = std::move(client_info)]() mutable {
                HandleConnection(connection, std::move(client_info));
            }).detach();
        }
        catch (const std::system_error &)
        {
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            close(connection);
        }
    }
}

void TcpServer::HandleConnection(int socket_fd, std::string client_info)
{
    ConnectionGuard guard(socket_fd, &active_connections_);
    timeval timeout{};
    timeout.tv_sec = receive_timeout_seconds_;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) == -1)
        return;

    char buffer[4096];
    std::string message;
    for (;;)
    {
        ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return;
        if (count == 0)
            break;
        if (message.size() + static_cast<size_t>(count) > kMaxMessageSize)
            return;
        message.append(buffer, static_cast<size_t>(count));
    }

    if (!expected_token_.empty())
    {
        size_t delimiter = message.find('\n');
        if (delimiter == std::string::npos ||
            message.substr(0, delimiter) != expected_token_)
            return;
        message.erase(0, delimiter + 1);
    }

    if (!message.empty())
        callback_(client_info + " " + message);
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3)
    {
        std::cerr << "usage: " << argv[0] << " <port> [listen-address]"
                  << std::endl;
        return 2;
    }

    unsigned port_value = 0;
    const char *port_end = argv[1] + std::strlen(argv[1]);
    auto parse_result = std::from_chars(argv[1], port_end, port_value);
    if (parse_result.ec != std::errc() || parse_result.ptr != port_end ||
        port_value == 0 || port_value > 65535)
    {
        std::cerr << "invalid port" << std::endl;
        return 2;
    }

    const std::string listen_address = argc == 3 ? argv[2] : "127.0.0.1";
    const char *token_env = std::getenv("ASYNLOG_BACKUP_TOKEN");
    const std::string expected_token = token_env == nullptr ? "" : token_env;
    const char *file_env = std::getenv("ASYNLOG_BACKUP_FILE");
    const std::string filename = file_env == nullptr ? "./logfile.log" : file_env;

    BackupFileWriter writer(filename);
    try
    {
        TcpServer server(static_cast<uint16_t>(port_value), listen_address,
                         [&writer](const std::string &message) {
                             if (!writer.Write(message))
                                 std::cerr << "write backup log failed" << std::endl;
                         },
                         expected_token);
        if (!server.Init())
        {
            std::cerr << "initialize backup server failed: " << strerror(errno)
                      << std::endl;
            return 1;
        }
        server.Run();
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
