#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class TcpServer
{
public:
    using MessageCallback = std::function<void(const std::string &)>;

    TcpServer(uint16_t port, std::string listen_address, MessageCallback callback,
              std::string expected_token = "", size_t max_connections = 64,
              int receive_timeout_seconds = 5);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    bool Init();
    void Run();

private:
    void HandleConnection(int socket_fd, std::string client_info);

private:
    static constexpr int kBacklog = 32;
    static constexpr size_t kMaxMessageSize = 1024 * 1024;

    int listen_socket_ = -1;
    uint16_t port_;
    std::string listen_address_;
    MessageCallback callback_;
    std::string expected_token_;
    size_t max_connections_;
    int receive_timeout_seconds_;
    std::atomic<size_t> active_connections_{0};
};
