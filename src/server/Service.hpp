#pragma once
#include "DataManager.hpp"

#include <sys/queue.h>
#include <event.h>
// for http
#include <evhttp.h>
#include <event2/http.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <cerrno>
#include <regex>

#include "base64.h" // 来自 cpp-base64 库
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>

extern storage::DataManager *data_;
namespace storage
{
    class Service
    {
    public:
        Service()
        {
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("Service start(Construct)");
#endif
            server_port_ = Config::GetInstance()->GetServerPort();
            server_ip_ = Config::GetInstance()->GetServerIp();
            download_prefix_ = Config::GetInstance()->GetDownloadPrefix();
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("Service end(Construct)");
#endif
        }
        bool RunModule()
        {
            // 初始化环境
            event_base *base = event_base_new();
            if (base == NULL)
            {
                mylog::GetLogger("asynclogger")->Fatal("event_base_new err!");
                return false;
            }
            // 设置监听的端口和地址
            sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(server_port_);
            // http 服务器,创建evhttp上下文
            evhttp *httpd = evhttp_new(base);
            // 绑定端口和ip
            if (evhttp_bind_socket(httpd, "0.0.0.0", server_port_) != 0)
            {
                mylog::GetLogger("asynclogger")->Fatal("evhttp_bind_socket failed!");
                return false;
            }
            // 设定回调函数
            // 指定generic callback，也可以为特定的URI指定callback
            evhttp_set_gencb(httpd, GenHandler, NULL);

            if (base)
            {
#ifdef DEBUG_LOG
                mylog::GetLogger("asynclogger")->Debug("event_base_dispatch");
#endif
                if (-1 == event_base_dispatch(base))
                {
                    mylog::GetLogger("asynclogger")->Debug("event_base_dispatch err");
                }
            }
            if (base)
                event_base_free(base);
            if (httpd)
                evhttp_free(httpd);
            return true;
        }

    private:
        uint16_t server_port_;
        std::string server_ip_;
        std::string download_prefix_;

    private:
        static void GenHandler(struct evhttp_request *req, void *arg)
        {
            std::string path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            path = UrlDecode(path);
            mylog::GetLogger("asynclogger")->Info("get req, uri: %s", path.c_str());

            // 统一鉴权：除了登录页面和登录接口，其他所有接口都需要验证
            bool is_login_related = (path == "/login" || path == "/api/login" || path == "/logout");
            if (!is_login_related) {
                if (!CheckAuth(req)) {
                    // 如果是页面请求，重定向到登录页
                    if (path == "/" || path.find("/download/") != std::string::npos) {
                        mylog::GetLogger("asynclogger")->Info("Unauthorized %s, Redirect to login", path.c_str());
                        evhttp_add_header(req->output_headers, "Location", "/login");
                        evhttp_send_reply(req, HTTP_MOVETEMP, "Redirect", NULL);
                    } else {
                        // 如果是 API 请求（上传/删除），返回 401
                        mylog::GetLogger("asynclogger")->Info("Unauthorized API call %s", path.c_str());
                        evhttp_send_reply(req, 401, "Unauthorized", NULL);
                    }
                    return;
                }
            }

            // 根据请求中的内容判断是什么请求
            // 这里是下载请求
            if (path.find("/download/") != std::string::npos)
            {
                Download(req, arg);
            }
            // 这里是上传
            else if (path == "/upload")
            {
                Upload(req, arg);
            }
            // 这里是删除
            else if (path == "/delete")
            {
                Delete(req, arg);
            }
            // 这里就是显示已存储文件列表，返回一个html页面给浏览器
            else if (path == "/")
            {
                ListShow(req, arg);
            }
            // 登录页面
            else if (path == "/login")
            {
                LoginShow(req, arg);
            }
            // 登录验证接口
            else if (path == "/api/login")
            {
                LoginAuth(req, arg);
            }
            // 退出登录
            else if (path == "/logout")
            {
                Logout(req, arg);
            }
            else
            {
                evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", NULL);
            }
        }

        // 使用简单哈希算法(SHA256)加密密码
        static std::string ComputeSHA256(const std::string& str) {
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256_CTX sha256;
            SHA256_Init(&sha256);
            SHA256_Update(&sha256, str.c_str(), str.length());
            SHA256_Final(hash, &sha256);
            std::stringstream ss;
            for(int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            {
                ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
            }
            return ss.str();
        }

        // 检查请求是否包含正确的 Cookie 或 Header
        static bool CheckAuth(struct evhttp_request *req) {
            // 登录页面和登录接口不需要验证
            std::string path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            if (path == "/login" || path == "/api/login") {
                return true;
            }

            std::string expected_hash = Config::GetInstance()->GetAuthHash();

            // 1. 检查浏览器访问的 Cookie
            const char* cookie = evhttp_find_header(req->input_headers, "Cookie");
            // Cookie 中存储的就是 Hash 值
            if (cookie && std::string(cookie).find("auth=" + expected_hash) != std::string::npos) {
                return true;
            }

            // 2. 检查 Header 中的 Password (API 使用)
            const char* pwd_header = evhttp_find_header(req->input_headers, "Password");
            if (pwd_header) {
                std::string salt = Config::GetInstance()->GetAuthSalt();
                // 验证: SHA256(header_pwd + salt) == stored_hash
                if (ComputeSHA256(std::string(pwd_header) + salt) == expected_hash) {
                    return true;
                }
            }

            return false;
        }

        static void LoginShow(struct evhttp_request *req, void *arg) {
            std::ifstream loginFile("login.html");
            if (!loginFile.is_open()) {
                evhttp_send_reply(req, HTTP_NOTFOUND, "login.html not found", NULL);
                return;
            }
            std::string content((std::istreambuf_iterator<char>(loginFile)), std::istreambuf_iterator<char>());
            evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8");
            evbuffer_add(evhttp_request_get_output_buffer(req), content.c_str(), content.size());
            evhttp_send_reply(req, HTTP_OK, "OK", NULL);
        }

        static void LoginAuth(struct evhttp_request *req, void *arg) {
            // 从 Header 获取密码
            const char* pwd = evhttp_find_header(req->input_headers, "Password");
            
            std::string salt = Config::GetInstance()->GetAuthSalt();
            std::string expected_hash = Config::GetInstance()->GetAuthHash();

            if (pwd && ComputeSHA256(std::string(pwd) + salt) == expected_hash) {
                // 登录成功，设置 Cookie
                // 现在 Cookie 直接存储 Hash 值，不再需要在运行时重新计算明文密码的 Hash
                
                std::string cookie_val = "auth=" + expected_hash + "; Path=/";
                evhttp_add_header(req->output_headers, "Set-Cookie", cookie_val.c_str());
                evhttp_send_reply(req, HTTP_OK, "Login Success", NULL);
            } else {
                evhttp_send_reply(req, 401, "Auth Failed", NULL);
            }
        }

        static void Logout(struct evhttp_request *req, void *arg) {
            // 设置过期时间为过去的时间，清除 Cookie
            std::string cookie_val = "auth=; Max-Age=0; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT";
            evhttp_add_header(req->output_headers, "Set-Cookie", cookie_val.c_str());
            
            // 防止重定向页面被缓存
            evhttp_add_header(req->output_headers, "Cache-Control", "no-cache, no-store, must-revalidate");
            evhttp_add_header(req->output_headers, "Pragma", "no-cache");
            evhttp_add_header(req->output_headers, "Expires", "0");

            // 重定向到登录页
            evhttp_add_header(req->output_headers, "Location", "/login");
            evhttp_send_reply(req, HTTP_MOVETEMP, "Logged Out", NULL);
        }

        static void Upload(struct evhttp_request *req, void *arg)
        {
            mylog::GetLogger("asynclogger")->Info("Upload start");

            // 约定：请求中包含"low_storage"，说明请求中存在文件数据,并希望普通存储
            // 包含"deep_storage"字段则压缩后存储
            // CheckAuth 已在 GenHandler 中统一调用
            // 获取请求体内容
            struct evbuffer *buf = evhttp_request_get_input_buffer(req);
            
            if (buf == nullptr)
            if (buf == nullptr)
            {
                mylog::GetLogger("asynclogger")->Info("evhttp_request_get_input_buffer is empty");
                return;
            }

            size_t len = evbuffer_get_length(buf); // 获取请求体的长度
            mylog::GetLogger("asynclogger")->Info("evbuffer_get_length is %u", len);
            if (0 == len)
            {
                evhttp_send_reply(req, HTTP_BADREQUEST, "file empty", NULL);
                mylog::GetLogger("asynclogger")->Info("request body is empty");
                return;
            }
            std::string content;
            content.resize(len);
            if (-1 == evbuffer_copyout(buf, (void *)&content[0], len))
            {
                mylog::GetLogger("asynclogger")->Error("evbuffer_copyout error");
                evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
                return;
            }

            // 获取文件名
            std::string filename = evhttp_find_header(req->input_headers, "FileName");
            // 解码文件名
            filename = base64_decode(filename);

            // 获取存储类型，客户端自定义请求头 StorageType
            std::string storage_type = evhttp_find_header(req->input_headers, "StorageType");
            // 组织存储路径
            std::string storage_path;
            if (storage_type == "low")
            {
                storage_path = Config::GetInstance()->GetLowStorageDir();
            }
            else if (storage_type == "deep")
            {
                storage_path = Config::GetInstance()->GetDeepStorageDir();
            }
            else
            {
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_BADREQUEST");
                evhttp_send_reply(req, HTTP_BADREQUEST, "Illegal storage type", NULL);
                return;
            }

            // 如果不存在就创建low或deep目录
            FileUtil dirCreate(storage_path);
            dirCreate.CreateDirectory();

            // 目录创建后加可以加上文件名，这个就是最终要写入的文件路径
            storage_path += filename;
#ifdef DEBUG_LOG
            mylog::GetLogger("asynclogger")->Debug("storage_path:%s", storage_path.c_str());
#endif

            // 看路径里是low还是deep存储，是deep就压缩，是low就直接写入
            FileUtil fu(storage_path);
            if (storage_path.find("low_storage") != std::string::npos)
            {
                if (fu.SetContent(content.c_str(), len) == false)
                {
                    mylog::GetLogger("asynclogger")->Error("low_storage fail, evhttp_send_reply: HTTP_INTERNAL");
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                else
                {
                    mylog::GetLogger("asynclogger")->Info("low_storage success");
                }
            }
            else
            {
                if (fu.Compress(content, Config::GetInstance()->GetBundleFormat()) == false)
                {
                    mylog::GetLogger("asynclogger")->Error("deep_storage fail, evhttp_send_reply: HTTP_INTERNAL");
                    evhttp_send_reply(req, HTTP_INTERNAL, "server error", NULL);
                    return;
                }
                else
                {
                    mylog::GetLogger("asynclogger")->Info("deep_storage success");
                }
            }

            // 添加存储文件信息，交由数据管理类进行管理
            StorageInfo info;
            info.NewStorageInfo(storage_path); // 组织存储的文件信息
            data_->Insert(info);               // 向数据管理模块添加存储的文件信息

            evhttp_send_reply(req, HTTP_OK, "Success", NULL);
            mylog::GetLogger("asynclogger")->Info("upload finish:success");
        }

        static void Delete(struct evhttp_request *req, void *arg)
        {
            mylog::GetLogger("asynclogger")->Info("Delete start");
            
            if (CheckAuth(req) == false) {
                 evhttp_send_reply(req, 401, "Unauthorized", NULL);
                 return;
            }

            // 1. 获取要删除的 url
            const char* url_cstr = evhttp_find_header(req->input_headers, "DeleteUrl");
            // url 可能是 encode 过的，需要 decode 吗？前端传过来的可能是原始的路径
            // 这里假设前端传过来的是 /download/filename

            if (url_cstr == NULL || strlen(url_cstr) == 0)
            {
                mylog::GetLogger("asynclogger")->Info("DeleteUrl header empty");
                evhttp_send_reply(req, HTTP_BADREQUEST, "DeleteUrl empty", NULL);
                return;
            }
            std::string url = url_cstr;
            // 前端传过来的是 URI encode 过的，需要 decode 还原回原始路径，才能在 map 中找到 key
            url = UrlDecode(url);

            // 2. 根据 url 获取文件信息，为了拿到 storage_path 进行物理删除
            StorageInfo info;
            if (data_->GetOneByURL(url, &info) == false)
            {
                mylog::GetLogger("asynclogger")->Info("File not found in DataManager: %s", url.c_str());
                evhttp_send_reply(req, HTTP_NOTFOUND, "File not found", NULL);
                return;
            }

            // 3. 物理删除文件
            if (remove(info.storage_path_.c_str()) != 0)
            {
                 mylog::GetLogger("asynclogger")->Error("remove file failed: %s, errno: %d", info.storage_path_.c_str(), errno);
                 // 物理删除失败，是否继续删除元数据？通常需要，避免元数据和文件不一致
            } 
            else 
            {
                mylog::GetLogger("asynclogger")->Info("remove file success: %s", info.storage_path_.c_str());
            }

            // 4. 从 DataManager 中删除元数据
            if (data_->Delete(url))
            {
                mylog::GetLogger("asynclogger")->Info("Delete Meta Success: %s", url.c_str());
                evhttp_send_reply(req, HTTP_OK, "Delete Success", NULL);
            }
            else
            {
                mylog::GetLogger("asynclogger")->Error("Delete Meta Fail: %s", url.c_str());
                evhttp_send_reply(req, HTTP_INTERNAL, "Delete Meta Fail", NULL);
            }
        }

        static std::string TimetoStr(time_t t)
        {
            std::string tmp = std::ctime(&t);
            return tmp;
        }

        // 前端代码处理函数
        // 在渲染函数中直接处理StorageInfo
        static std::string generateModernFileList(const std::vector<StorageInfo> &files)
        {
            std::stringstream ss;
            ss << "<div class='file-list'><h3>已上传文件</h3>";

            for (const auto &file : files)
            {
                std::string filename = FileUtil(file.storage_path_).FileName();

                // 从路径中解析存储类型（示例逻辑，需根据实际路径规则调整）
                std::string storage_type = "low";
                if (file.storage_path_.find("deep") != std::string::npos)
                {
                    storage_type = "deep";
                }

                ss << "<div class='file-item'>"
                   << "<div class='file-info'>"
                   << "<span>📄" << filename << "</span>"
                   << "<span class='file-type'>"
                   << (storage_type == "deep" ? "深度存储" : "普通存储")
                   << "</span>"
                   << "<span>" << formatSize(file.fsize_) << "</span>"
                   << "<span>" << TimetoStr(file.mtime_) << "</span>"
                   << "</div>"
                   << "<div style='display: flex; gap: 10px;'>"
                   << "<button onclick=\"window.location='" << file.url_ << "'\">⬇️ 下载</button>"
                   << "<button onclick=\"deleteFile('" << file.url_ << "')\" style='background-color: #e74c3c;'>🗑️ 删除</button>"
                   << "</div>"
                   << "</div>";
            }

            ss << "</div>";
            return ss.str();
        }

        // 文件大小格式化函数
        static std::string formatSize(uint64_t bytes)
        {
            const char *units[] = {"B", "KB", "MB", "GB"};
            int unit_index = 0;
            double size = bytes;

            while (size >= 1024 && unit_index < 3)
            {
                size /= 1024;
                unit_index++;
            }

            std::stringstream ss;
            ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
            return ss.str();
        }
        static void ListShow(struct evhttp_request *req, void *arg)
        {
            mylog::GetLogger("asynclogger")->Info("ListShow()");
            // 避免浏览器缓存 index.html，导致退出登录后回退依然看到页面
            evhttp_add_header(req->output_headers, "Cache-Control", "no-cache, no-store, must-revalidate");
            evhttp_add_header(req->output_headers, "Pragma", "no-cache");
            evhttp_add_header(req->output_headers, "Expires", "0");

            if (!CheckAuth(req)) {
                 mylog::GetLogger("asynclogger")->Info("Redirect to login");
                 evhttp_add_header(req->output_headers, "Location", "/login");
                 evhttp_send_reply(req, HTTP_MOVETEMP, "Redirect", NULL);
                 return;
            }
            // 1. 获取所有的文件存储信息
            std::vector<StorageInfo> arry;
            data_->GetAll(&arry);

            // 读取模板文件
            std::ifstream templateFile("index.html");
            std::string templateContent(
                (std::istreambuf_iterator<char>(templateFile)),
                std::istreambuf_iterator<char>());

            // 替换html文件中的占位符
            //替换文件列表进html
            templateContent = std::regex_replace(templateContent,
                                                 std::regex("\\{\\{FILE_LIST\\}\\}"),
                                                 generateModernFileList(arry));
            //替换服务器地址进hrml
            templateContent = std::regex_replace(templateContent,
                                                 std::regex("\\{\\{BACKEND_URL\\}\\}"),
                                                "http://"+storage::Config::GetInstance()->GetServerIp()+":"+std::to_string(storage::Config::GetInstance()->GetServerPort()));
            // 获取请求的输出evbuffer
            struct evbuffer *buf = evhttp_request_get_output_buffer(req);
            auto response_body = templateContent;
            // 把前面的html数据给到evbuffer，然后设置响应头部字段，最后返回给浏览器
            evbuffer_add(buf, (const void *)response_body.c_str(), response_body.size());
            evhttp_add_header(req->output_headers, "Content-Type", "text/html;charset=utf-8");
            evhttp_send_reply(req, HTTP_OK, NULL, NULL);
            mylog::GetLogger("asynclogger")->Info("ListShow() finish");
        }
        static std::string GetETag(const StorageInfo &info)
        {
            // 自定义etag :  filename-fsize-mtime
            FileUtil fu(info.storage_path_);
            std::string etag = fu.FileName();
            etag += "-";
            etag += std::to_string(info.fsize_);
            etag += "-";
            etag += std::to_string(info.mtime_);
            return etag;
        }
        static void Download(struct evhttp_request *req, void *arg)
        {


            // 1. 获取客户端请求的资源路径path   req.path
            // 2. 根据资源路径，获取StorageInfo
            StorageInfo info;
            std::string resource_path = evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req));
            resource_path = UrlDecode(resource_path);
            data_->GetOneByURL(resource_path, &info);
            mylog::GetLogger("asynclogger")->Info("request resource_path:%s", resource_path.c_str());

            std::string download_path = info.storage_path_;
            // 2.如果压缩过了就解压到新文件给用户下载
            if (info.storage_path_.find(Config::GetInstance()->GetLowStorageDir()) == std::string::npos)
            {
                mylog::GetLogger("asynclogger")->Info("uncompressing:%s", info.storage_path_.c_str());
                FileUtil fu(info.storage_path_);
                download_path = Config::GetInstance()->GetLowStorageDir() +
                                std::string(download_path.begin() + download_path.find_last_of('/') + 1, download_path.end());
                FileUtil dirCreate(Config::GetInstance()->GetLowStorageDir());
                dirCreate.CreateDirectory();
                fu.UnCompress(download_path); // 将文件解压到low_storage下去或者再创一个文件夹做中转
            }
            mylog::GetLogger("asynclogger")->Info("request download_path:%s", download_path.c_str());
            FileUtil fu(download_path);
            if (fu.Exists() == false && info.storage_path_.find("deep_storage") != std::string::npos)
            {
                // 如果是压缩文件，且解压失败，是服务端的错误
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 500 - UnCompress failed");
                evhttp_send_reply(req, HTTP_INTERNAL, NULL, NULL);
            }

            //这个else if是没用的，不看就行
            else if (fu.Exists() == false && info.storage_path_.find("low_storage") == std::string::npos)
            {
                // 如果是普通文件，且文件不存在，是客户端的错误
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 400 - bad request,file not exists");
                evhttp_send_reply(req, HTTP_BADREQUEST, "file not exists", NULL);
            }

            // 3.确认文件是否需要断点续传
            bool retrans = false;
            std::string old_etag;
            auto if_range = evhttp_find_header(req->input_headers, "If-Range");
            if (NULL != if_range)
            {
                old_etag = if_range;
                // 有If-Range字段且，这个字段的值与请求文件的最新etag一致则符合断点续传
                if (old_etag == GetETag(info))
                {
                    retrans = true;
                    mylog::GetLogger("asynclogger")->Info("%s need breakpoint continuous transmission", download_path.c_str());
                }
            }

            // 4.判断文件是否存在
            if (fu.Exists() == false)
            {
                mylog::GetLogger("asynclogger")->Info("%s not exists", download_path.c_str());
                download_path += "not exists";
                evhttp_send_reply(req, 404, download_path.c_str(), NULL);
                return;
            }
            //获取当前 HTTP 请求对象 (req) 内部关联的输出缓冲区 (output buffer) 的指针（地址）
            evbuffer *outbuf = evhttp_request_get_output_buffer(req);
            int fd = open(download_path.c_str(), O_RDONLY);
            if (fd == -1)
            {
                mylog::GetLogger("asynclogger")->Error("open file error: %s -- %s", download_path.c_str(), strerror(errno));
                evhttp_send_reply(req, HTTP_INTERNAL, strerror(errno), NULL);
                return;
            }

            // [新增] 解析 Range 头并确定发送范围
            long start_offset = 0;//开始位置默认为0
            long length = fu.FileSize();//发送长度默认为整个文件大小
            
            // 只有当符合续传条件（retrans=true）且存在 Range 头时才解析，
            // 或者如果没有 If-Range 头但有 Range 头（通常也应视为续传请求）
            auto range_header = evhttp_find_header(req->input_headers, "Range");
            if (range_header != NULL && (retrans || if_range == NULL)) 
            {
                std::string range_val = range_header;
                // Range 头的值，例如 "bytes=100-200" 或 "bytes=100-" 或 "bytes=-200"
                // 简单的 Range 解析: bytes=start-
                size_t eq_pos = range_val.find("=");
                size_t minus_pos = range_val.find("-");
                if (eq_pos != std::string::npos && minus_pos != std::string::npos) 
                {
                    long fsize = fu.FileSize();// 获取文件大小
                    try {
                        std::string start_str = range_val.substr(eq_pos + 1, minus_pos - eq_pos - 1);
                        //两个参数：start_pos是要提取的子字符串的起始位置，length是要提取的子字符串的长度
                        start_offset = std::stol(start_str);
                        
                        // 如果有 end，解析 end (bytes=start-end)
                        std::string end_str = range_val.substr(minus_pos + 1);
                        // 如果没有 end，默认为文件末尾 (bytes=start-)
                        long end_offset = fsize - 1;
                        if (!end_str.empty()) {
                             end_offset = std::stol(end_str);
                        }
                        
                        if (start_offset < fsize) {
                            if (end_offset >= fsize) end_offset = fsize - 1;
                            length = end_offset - start_offset + 1;
                            retrans = true; // 确保设置为 true 以发送 206
                        } else {
                            start_offset = 0;
                            length = fsize;
                            retrans = false; 
                        }
                    } catch (...) {
                        start_offset = 0;
                        length = fsize;
                        retrans = false; 
                    }
                }
            }

            // 使用 evbuffer_add_file 直接将文件内容添加到输出缓冲区，避免一次性读入内存
            if (-1 == evbuffer_add_file(outbuf, fd, start_offset, length))
            {
                mylog::GetLogger("asynclogger")->Error("evbuffer_add_file: %d -- %s -- %s", fd, download_path.c_str(), strerror(errno));
            }
            // 5. 设置响应头部字段： ETag， Accept-Ranges: bytes
            evhttp_add_header(req->output_headers, "Accept-Ranges", "bytes");
            evhttp_add_header(req->output_headers, "ETag", GetETag(info).c_str());
            evhttp_add_header(req->output_headers, "Content-Type", "application/octet-stream");
            if (retrans == false)
            {
                evhttp_send_reply(req, HTTP_OK, "Success", NULL);
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: HTTP_OK");
            }
            else
            {
                std::string content_range = "bytes " + std::to_string(start_offset) + "-" + std::to_string(start_offset + length - 1) + "/" + std::to_string(fu.FileSize());
                evhttp_add_header(req->output_headers, "Content-Range", content_range.c_str());
                evhttp_send_reply(req, 206, "Partial Content", NULL); // 区间请求响应的是206
                mylog::GetLogger("asynclogger")->Info("evhttp_send_reply: 206");
            }
            if (download_path != info.storage_path_)
            {
                remove(download_path.c_str()); // 删除文件
            }
        }
    };
}
