# AsynLogSystem-CloudStorage

这是一个 C++17 单机云存储练习项目，包含 HTTP 文件服务、普通/压缩存储、JSON 元数据持久化、异步日志，以及可独立运行的远程日志备份服务。

## 主要模块

- `src/server`：登录、文件上传/下载/删除、Range/HEAD 响应和元数据管理。
- `log_system/logs_code`：双缓冲异步日志、滚动文件、线程池和远程备份客户端。
- `log_system/logs_code/backlog`：远程日志备份接收端。

项目保留 JSON 快照作为元数据存储；当前规模下没有引入数据库，也没有实现完整分片或断点上传协议。

## 构建依赖

- 支持 C++17 的 `g++`
- pthread
- jsoncpp
- libevent 2.x
- OpenSSL
- `bundle` 2.1.0 动态库（仓库内已保留对应的 `bundle.h`）

在 Ubuntu/Debian 上，除 `bundle` 外的常见依赖可通过以下命令安装：

```bash
sudo apt install build-essential libjsoncpp-dev libevent-dev libssl-dev
```

`libbundle.so` 需要单独编译安装，并确保动态链接器可以找到它，例如安装到 `/usr/local/lib` 后执行 `sudo ldconfig`。

## 编译与测试

```bash
cd src/server
make all
make tests
```

构建产物位于 `src/server/build/bin`：

- `cloud_storage_server`
- `backup_server`
- `log_example`
- `benchmark`
- `unit_tests`（执行 `make tests` 时构建）

`make clean` 只删除 `build` 目录，不会删除已上传文件或元数据。确实需要清空默认运行数据时，必须显式执行：

```bash
make purge-data CONFIRM=yes
```

## 配置

文件服务默认读取 `src/server/Storage.conf`，也可以通过命令行参数或环境变量指定：

```bash
./build/bin/cloud_storage_server /absolute/path/to/Storage.conf
CLOUD_STORAGE_CONFIG=/absolute/path/to/Storage.conf ./build/bin/cloud_storage_server
```

日志模块默认查找 `log_system/logs_code/config.conf`，可用 `ASYNLOG_CONFIG` 覆盖。认证盐和密码摘要还可分别用 `CLOUD_STORAGE_AUTH_SALT`、`CLOUD_STORAGE_AUTH_HASH` 覆盖，适合部署时避免把真实凭据写入仓库。

默认监听地址为 `127.0.0.1:8081`，默认演示密码为 `admin`。公开部署前必须修改认证盐和摘要，并根据 HTTPS 部署情况把 `cookie_secure` 设为 `true`。

密码摘要算法是 PBKDF2-HMAC-SHA256。下面的命令可生成新摘要，盐和迭代次数要与 `Storage.conf` 一致：

```bash
python3 -c 'import getpass,hashlib; print(hashlib.pbkdf2_hmac("sha256", getpass.getpass("Password: ").encode(), b"YOUR_SALT", 120000).hex())'
```

## 运行

建议从 `src/server` 目录启动，因为当前 HTML 模板使用该目录下的相对路径：

```bash
cd src/server
./build/bin/cloud_storage_server Storage.conf
```

浏览器访问 `http://127.0.0.1:8081`。

远程日志备份服务默认只监听回环地址：

```bash
./build/bin/backup_server 8080
```

跨机器部署时可以传入监听地址，并用共享 token 限制写入来源：

```bash
ASYNLOG_BACKUP_TOKEN='replace-me' ./build/bin/backup_server 8080 0.0.0.0
```

客户端配置中的 `backup_token` 必须与服务端环境变量一致。备份文件可通过 `ASYNLOG_BACKUP_FILE` 指定；单文件达到 100 MB 后滚动，默认保留 5 份。

## HTTP 行为

- `GET /login`：登录页。
- `POST /api/login`：校验密码并建立服务端会话。
- `GET /`：文件列表。
- `POST /upload`：上传文件，需要 session 与 CSRF token；同名文件返回 409，不覆盖原文件。
- `GET|HEAD /download/<filename>`：下载，支持单段 Range。
- `POST /delete`：删除文件，需要 session 与 CSRF token。
- `POST /logout`：注销当前会话。

服务端限制 Header、请求体、文件名和上传大小。语法无法识别的 Range 会被忽略并回退完整 200；明确不可满足的 Range 返回 416。
