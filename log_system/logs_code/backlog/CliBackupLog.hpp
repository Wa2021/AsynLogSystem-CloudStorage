#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../Util.hpp"

extern mylog::Util::JsonData *g_conf_data;

namespace mylog
{
    namespace backup
    {
        inline bool ConnectWithTimeout(int socket_fd, const sockaddr *address,
                                       socklen_t address_length, int timeout_ms)
        {
            int old_flags = fcntl(socket_fd, F_GETFL, 0);
            if (old_flags == -1 || fcntl(socket_fd, F_SETFL, old_flags | O_NONBLOCK) == -1)
                return false;

            int result = connect(socket_fd, address, address_length);
            if (result == 0)
            {
                return fcntl(socket_fd, F_SETFL, old_flags) == 0;
            }
            if (errno != EINPROGRESS)
            {
                int saved_errno = errno;
                fcntl(socket_fd, F_SETFL, old_flags);
                errno = saved_errno;
                return false;
            }

            pollfd descriptor{};
            descriptor.fd = socket_fd;
            descriptor.events = POLLOUT;
            do
            {
                result = poll(&descriptor, 1, timeout_ms);
            } while (result < 0 && errno == EINTR);

            if (result <= 0)
            {
                int saved_errno = result == 0 ? ETIMEDOUT : errno;
                fcntl(socket_fd, F_SETFL, old_flags);
                errno = saved_errno;
                return false;
            }

            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                           &error_length) == -1)
            {
                int saved_errno = errno;
                fcntl(socket_fd, F_SETFL, old_flags);
                errno = saved_errno;
                return false;
            }

            if (fcntl(socket_fd, F_SETFL, old_flags) == -1)
                return false;
            if (socket_error != 0)
            {
                errno = socket_error;
                return false;
            }
            return true;
        }

        inline bool Start(const std::string &message)
        {
            if (g_conf_data == nullptr || !g_conf_data->backup_enabled)
                return true;

            sockaddr_in server{};
            server.sin_family = AF_INET;
            server.sin_port = htons(g_conf_data->backup_port);
            if (inet_pton(AF_INET, g_conf_data->backup_addr.c_str(),
                          &server.sin_addr) != 1)
            {
                std::cerr << "invalid backup address: " << g_conf_data->backup_addr
                          << std::endl;
                return false;
            }

            std::string payload;
            if (!g_conf_data->backup_token.empty())
                payload = g_conf_data->backup_token + "\n" + message;
            else
                payload = message;

            for (int attempt = 0; attempt < g_conf_data->backup_retries; ++attempt)
            {
                int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (socket_fd < 0)
                    return false;

                timeval timeout{};
                timeout.tv_sec = g_conf_data->backup_send_timeout_ms / 1000;
                timeout.tv_usec =
                    (g_conf_data->backup_send_timeout_ms % 1000) * 1000;
                if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                               sizeof(timeout)) == -1)
                {
                    close(socket_fd);
                    return false;
                }

                if (!ConnectWithTimeout(socket_fd,
                                        reinterpret_cast<sockaddr *>(&server),
                                        sizeof(server),
                                        g_conf_data->backup_connect_timeout_ms))
                {
                    close(socket_fd);
                    continue;
                }

                size_t total = 0;
                bool success = true;
                while (total < payload.size())
                {
                    ssize_t sent = send(socket_fd, payload.data() + total,
                                        payload.size() - total, MSG_NOSIGNAL);
                    if (sent < 0 && errno == EINTR)
                        continue;
                    if (sent <= 0)
                    {
                        success = false;
                        break;
                    }
                    total += static_cast<size_t>(sent);
                }

                shutdown(socket_fd, SHUT_WR);
                close(socket_fd);
                if (success)
                    return true;
            }

            return false;
        }
    } // namespace backup
} // namespace mylog
