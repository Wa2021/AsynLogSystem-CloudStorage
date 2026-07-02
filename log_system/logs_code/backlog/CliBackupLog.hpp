// 远程备份debug等级以上的日志信息-发送端
#include <iostream>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include "../Util.hpp"

extern mylog::Util::JsonData *g_conf_data;

static bool connect_with_timeout(int sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms)
{
    int old_flags = fcntl(sock, F_GETFL, 0);
    if (old_flags == -1)
        return connect(sock, addr, addrlen) == 0;

    if (fcntl(sock, F_SETFL, old_flags | O_NONBLOCK) == -1)
        return connect(sock, addr, addrlen) == 0;

    int ret = connect(sock, addr, addrlen);
    if (ret == 0)
    {
        fcntl(sock, F_SETFL, old_flags);
        return true;
    }
    if (errno != EINPROGRESS)
    {
        fcntl(sock, F_SETFL, old_flags);
        return false;
    }

    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
    {
        fcntl(sock, F_SETFL, old_flags);
        if (ret == 0)
            errno = ETIMEDOUT;
        return false;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) == -1)
    {
        fcntl(sock, F_SETFL, old_flags);
        return false;
    }

    fcntl(sock, F_SETFL, old_flags);
    if (so_error != 0)
    {
        errno = so_error;
        return false;
    }
    return true;
}

void start_backup(const std::string &message)
{
    // 1. create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        std::cout << __FILE__ << ":" << __LINE__ << " socket error : " << strerror(errno) << std::endl;
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(g_conf_data->backup_port);
    inet_aton(g_conf_data->backup_addr.c_str(), &(server.sin_addr));

    int cnt = 3;
    while (!connect_with_timeout(sock, (struct sockaddr *)&server, sizeof(server), 1000))
    {
        std::cout << "无法连接备份服务器，正在尝试重连, 重连次数剩余: " << cnt-- << std::endl;
        if (cnt <= 0)
        {
            std::cout << __FILE__ << ":" << __LINE__ << " connect error : " << strerror(errno) << std::endl;
            close(sock);
            return;
        }
        close(sock);
        sleep(1);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            std::cout << __FILE__ << ":" << __LINE__ << " socket error : " << strerror(errno) << std::endl;
            return;
        }
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    // 3. 连接成功后循环发送，避免只写出部分数据
    size_t total = 0;
    while (total < message.size())
    {
        ssize_t n = write(sock, message.c_str() + total, message.size() - total);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            std::cout << __FILE__ << ":" << __LINE__ << " send to server error : " << strerror(errno) << std::endl;
            close(sock);
            return;
        }
        total += static_cast<size_t>(n);
    }
    close(sock);
}
