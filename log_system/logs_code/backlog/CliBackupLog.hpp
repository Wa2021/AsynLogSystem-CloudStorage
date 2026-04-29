// 远程备份debug等级以上的日志信息-发送端
#include <iostream>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include "../Util.hpp"

extern mylog::Util::JsonData *g_conf_data;
void start_backup(const std::string &message)
{
    // 1. create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        std::cout << __FILE__ << ":" << __LINE__ << " socket error : " << strerror(errno) << std::endl;
        return;
    }

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(g_conf_data->backup_port);
    inet_aton(g_conf_data->backup_addr.c_str(), &(server.sin_addr));

    int cnt = 5;
    while (-1 == connect(sock, (struct sockaddr *)&server, sizeof(server)))
    {
        std::cout << "无法连接备份服务器，正在尝试重连, 重连次数剩余: " << cnt-- << std::endl;
        if (cnt <= 0)
        {
            std::cout << __FILE__ << ":" << __LINE__ << " connect error : " << strerror(errno) << std::endl;
            close(sock);
            return;
        }
        sleep(1);
    }

    // 3. 连接成功后循环发送，避免只写出部分数据
    size_t total = 0;
    while (total < message.size())
    {
        ssize_t n = write(sock, message.c_str() + total, message.size() - total);
        if (n <= 0)
        {
            std::cout << __FILE__ << ":" << __LINE__ << " send to server error : " << strerror(errno) << std::endl;
            close(sock);
            return;
        }
        total += static_cast<size_t>(n);
    }
    close(sock);
}
