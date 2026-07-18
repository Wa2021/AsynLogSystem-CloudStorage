#include "Service.hpp"

#include <event2/buffer.h>
#include <event2/event.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <unistd.h>

#include "Config.hpp"
#include "Range.hpp"
#include "Util.hpp"
#include "base64.h"

namespace
{
    using Clock = std::chrono::system_clock;
    using storage::Config;
    using storage::FileUtil;
    using storage::UrlDecode;
    using storage::UrlEncode;

    struct SessionRecord
    {
        std::string csrf_token;
        Clock::time_point expires_at;
    };

    struct AuthContext
    {
        std::string session_token;
        SessionRecord session;
    };

    class SessionStore
    {
    public:
        bool Create(int ttl_seconds, std::string *session_token,
                    std::string *csrf_token)
        {
            if (session_token == nullptr || csrf_token == nullptr)
                return false;

            std::string session;
            std::string csrf;
            if (!RandomToken(&session) || !RandomToken(&csrf))
                return false;

            std::lock_guard<std::mutex> lock(mutex_);
            RemoveExpiredLocked();
            sessions_[session] =
                SessionRecord{csrf, Clock::now() + std::chrono::seconds(ttl_seconds)};
            *session_token = std::move(session);
            *csrf_token = std::move(csrf);
            return true;
        }

        bool Validate(const std::string &token, SessionRecord *record)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto iterator = sessions_.find(token);
            if (iterator == sessions_.end())
                return false;
            if (iterator->second.expires_at <= Clock::now())
            {
                sessions_.erase(iterator);
                return false;
            }
            if (record != nullptr)
                *record = iterator->second;
            return true;
        }

        void Remove(const std::string &token)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.erase(token);
        }

    private:
        static bool RandomToken(std::string *token)
        {
            std::array<unsigned char, 32> bytes{};
            if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
                return false;
            static const char hex[] = "0123456789abcdef";
            token->clear();
            token->reserve(bytes.size() * 2);
            for (unsigned char byte : bytes)
            {
                token->push_back(hex[byte >> 4]);
                token->push_back(hex[byte & 0x0f]);
            }
            return true;
        }

        void RemoveExpiredLocked()
        {
            const Clock::time_point now = Clock::now();
            for (auto iterator = sessions_.begin(); iterator != sessions_.end();)
            {
                if (iterator->second.expires_at <= now)
                    iterator = sessions_.erase(iterator);
                else
                    ++iterator;
            }
        }

    private:
        std::mutex mutex_;
        std::unordered_map<std::string, SessionRecord> sessions_;
    };

    class LoginRateLimiter
    {
    public:
        bool Allow(const std::string &address)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto iterator = attempts_.find(address);
            if (iterator == attempts_.end())
                return true;
            const auto now = Clock::now();
            if (iterator->second.blocked_until > now)
                return false;
            if (now - iterator->second.window_started > std::chrono::minutes(1))
                attempts_.erase(iterator);
            return true;
        }

        void Failure(const std::string &address)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = Clock::now();
            Attempt &attempt = attempts_[address];
            if (attempt.window_started.time_since_epoch().count() == 0 ||
                now - attempt.window_started > std::chrono::minutes(1))
            {
                attempt.window_started = now;
                attempt.failures = 0;
            }
            ++attempt.failures;
            if (attempt.failures >= 5)
                attempt.blocked_until = now + std::chrono::minutes(1);
        }

        void Success(const std::string &address)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            attempts_.erase(address);
        }

    private:
        struct Attempt
        {
            Clock::time_point window_started{};
            Clock::time_point blocked_until{};
            int failures = 0;
        };

        std::mutex mutex_;
        std::unordered_map<std::string, Attempt> attempts_;
    };

    class TempFileGuard
    {
    public:
        TempFileGuard() = default;
        explicit TempFileGuard(std::string path) : path_(std::move(path)) {}
        ~TempFileGuard()
        {
            if (!path_.empty())
                std::remove(path_.c_str());
        }

        TempFileGuard(const TempFileGuard &) = delete;
        TempFileGuard &operator=(const TempFileGuard &) = delete;

        const std::string &Path() const { return path_; }
        void Reset(std::string path) { path_ = std::move(path); }
        void Release() { path_.clear(); }

    private:
        std::string path_;
    };

    SessionStore &Sessions()
    {
        static SessionStore store;
        return store;
    }

    LoginRateLimiter &LoginLimiter()
    {
        static LoginRateLimiter limiter;
        return limiter;
    }

    evkeyvalq *InputHeaders(evhttp_request *request)
    {
        return evhttp_request_get_input_headers(request);
    }

    evkeyvalq *OutputHeaders(evhttp_request *request)
    {
        return evhttp_request_get_output_headers(request);
    }

    void AddHeader(evhttp_request *request, const char *name,
                   const std::string &value)
    {
        evhttp_add_header(OutputHeaders(request), name, value.c_str());
    }

    void AddSecurityHeaders(evhttp_request *request)
    {
        AddHeader(request, "X-Content-Type-Options", "nosniff");
        AddHeader(request, "X-Frame-Options", "DENY");
        AddHeader(request, "Referrer-Policy", "same-origin");
    }

    void SendText(evhttp_request *request, int status, const char *reason,
                  const std::string &body = "")
    {
        evhttp_remove_header(OutputHeaders(request), "Content-Length");
        if (!body.empty())
        {
            evbuffer *output = evhttp_request_get_output_buffer(request);
            evbuffer_add(output, body.data(), body.size());
            AddHeader(request, "Content-Type", "text/plain;charset=utf-8");
        }
        evhttp_send_reply(request, status, reason, nullptr);
    }

    void SendMethodNotAllowed(evhttp_request *request, const char *allowed)
    {
        AddHeader(request, "Allow", allowed);
        SendText(request, 405, "Method Not Allowed", "method not allowed");
    }

    void SetNoCache(evhttp_request *request)
    {
        AddHeader(request, "Cache-Control", "no-cache, no-store, must-revalidate");
        AddHeader(request, "Pragma", "no-cache");
        AddHeader(request, "Expires", "0");
    }

    std::string Trim(const std::string &input)
    {
        size_t begin = input.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return "";
        size_t end = input.find_last_not_of(" \t");
        return input.substr(begin, end - begin + 1);
    }

    bool GetCookie(evhttp_request *request, const std::string &name,
                   std::string *value)
    {
        const char *header = evhttp_find_header(InputHeaders(request), "Cookie");
        if (header == nullptr || value == nullptr)
            return false;

        std::string cookies = header;
        size_t position = 0;
        while (position <= cookies.size())
        {
            size_t end = cookies.find(';', position);
            std::string item = Trim(cookies.substr(
                position, end == std::string::npos ? std::string::npos
                                                   : end - position));
            size_t separator = item.find('=');
            if (separator != std::string::npos &&
                Trim(item.substr(0, separator)) == name)
            {
                *value = item.substr(separator + 1);
                return true;
            }
            if (end == std::string::npos)
                break;
            position = end + 1;
        }
        return false;
    }

    bool SecureEqual(const std::string &left, const std::string &right)
    {
        return left.size() == right.size() &&
               CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
    }

    bool Authenticate(evhttp_request *request, AuthContext *context = nullptr)
    {
        std::string session_token;
        if (!GetCookie(request, "session", &session_token))
            return false;
        SessionRecord record;
        if (!Sessions().Validate(session_token, &record))
            return false;
        if (context != nullptr)
            *context = AuthContext{std::move(session_token), std::move(record)};
        return true;
    }

    bool ValidateCsrf(evhttp_request *request, const AuthContext &context)
    {
        const char *header =
            evhttp_find_header(InputHeaders(request), "X-CSRF-Token");
        std::string cookie;
        return header != nullptr && GetCookie(request, "csrf", &cookie) &&
               SecureEqual(header, context.session.csrf_token) &&
               SecureEqual(cookie, context.session.csrf_token);
    }

    bool DerivePasswordHash(const std::string &password, std::string *hex_digest)
    {
        if (hex_digest == nullptr || password.size() > static_cast<size_t>(INT_MAX))
            return false;
        Config *config = Config::GetInstance();
        std::array<unsigned char, 32> digest{};
        const std::string &salt = config->GetAuthSalt();
        if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                              reinterpret_cast<const unsigned char *>(salt.data()),
                              static_cast<int>(salt.size()),
                              config->GetAuthIterations(), EVP_sha256(),
                              static_cast<int>(digest.size()), digest.data()) != 1)
            return false;

        static const char hex[] = "0123456789abcdef";
        hex_digest->clear();
        hex_digest->reserve(digest.size() * 2);
        for (unsigned char byte : digest)
        {
            hex_digest->push_back(hex[byte >> 4]);
            hex_digest->push_back(hex[byte & 0x0f]);
        }
        return true;
    }

    std::string Sha256Hex(const std::string &input)
    {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_length = 0;
        if (EVP_Digest(input.data(), input.size(), digest.data(), &digest_length,
                       EVP_sha256(), nullptr) != 1)
            return "";

        static const char hex[] = "0123456789abcdef";
        std::string output;
        output.reserve(digest_length * 2);
        for (unsigned int i = 0; i < digest_length; ++i)
        {
            output.push_back(hex[digest[i] >> 4]);
            output.push_back(hex[digest[i] & 0x0f]);
        }
        return output;
    }

    std::string PeerAddress(evhttp_request *request)
    {
        evhttp_connection *connection = evhttp_request_get_connection(request);
        char *address = nullptr;
        ev_uint16_t port = 0;
        if (connection != nullptr)
            evhttp_connection_get_peer(connection, &address, &port);
        return address == nullptr ? "unknown" : address;
    }

    std::string CookieSuffix(bool http_only)
    {
        Config *config = Config::GetInstance();
        std::string suffix = "; Path=/; SameSite=Strict; Max-Age=" +
                             std::to_string(config->GetSessionTtlSeconds());
        if (http_only)
            suffix += "; HttpOnly";
        if (config->GetCookieSecure())
            suffix += "; Secure";
        return suffix;
    }

    void ClearAuthCookies(evhttp_request *request)
    {
        std::string suffix = "; Path=/; SameSite=Strict; Max-Age=0";
        if (Config::GetInstance()->GetCookieSecure())
            suffix += "; Secure";
        AddHeader(request, "Set-Cookie", "session=" + suffix + "; HttpOnly");
        AddHeader(request, "Set-Cookie", "csrf=" + suffix);
    }

    bool ReadRequestBody(evhttp_request *request, size_t maximum,
                         std::string *body)
    {
        if (body == nullptr)
            return false;
        evbuffer *input = evhttp_request_get_input_buffer(request);
        if (input == nullptr)
            return false;
        size_t length = evbuffer_get_length(input);
        if (length == 0 || length > maximum)
            return false;
        body->assign(length, '\0');
        ev_ssize_t copied = evbuffer_copyout(input, body->data(), length);
        return copied >= 0 && static_cast<size_t>(copied) == length;
    }

    bool IsSafeFileName(const std::string &filename)
    {
        if (filename.empty() || filename.size() > 255 || filename == "." ||
            filename == "..")
            return false;
        for (unsigned char ch : filename)
        {
            if (ch == '/' || ch == '\\' || ch == 0 || ch < 0x20 || ch == 0x7f)
                return false;
        }
        return true;
    }

    std::string HtmlEscape(const std::string &text)
    {
        std::string escaped;
        escaped.reserve(text.size());
        for (char ch : text)
        {
            switch (ch)
            {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    std::string FormatSize(uint64_t bytes)
    {
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        size_t unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024.0 && unit + 1 < std::size(units))
        {
            size /= 1024.0;
            ++unit;
        }
        std::ostringstream output;
        output << std::fixed << std::setprecision(2) << size << ' ' << units[unit];
        return output.str();
    }

    std::string FormatTime(time_t time)
    {
        tm local_time{};
        localtime_r(&time, &local_time);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_time);
        return buffer;
    }

    bool ReplaceOnce(std::string *text, const std::string &placeholder,
                     const std::string &replacement)
    {
        if (text == nullptr)
            return false;
        size_t position = text->find(placeholder);
        if (position == std::string::npos)
            return false;
        text->replace(position, placeholder.size(), replacement);
        return true;
    }

    std::string EntityTag(const storage::StorageInfo &info)
    {
        return '"' + Sha256Hex(info.storage_path_ + ':' +
                                std::to_string(info.fsize_) + ':' +
                                std::to_string(info.mtime_)) + '"';
    }

    bool IsDownloadPath(const std::string &path)
    {
        const std::string &prefix = Config::GetInstance()->GetDownloadPrefix();
        return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
    }
} // namespace

namespace storage
{
    Service::Service(DataManager &data_manager) : data_manager_(data_manager) {}

    bool Service::RunModule()
    {
        event_base *base = event_base_new();
        if (base == nullptr)
            return false;

        evhttp *http = evhttp_new(base);
        if (http == nullptr)
        {
            event_base_free(base);
            return false;
        }

        Config *config = Config::GetInstance();
        evhttp_set_max_headers_size(http, 64 * 1024);
        evhttp_set_max_body_size(
            http, static_cast<ev_ssize_t>(config->GetMaxUploadSize()));
        evhttp_set_timeout(http, 30);
        evhttp_set_allowed_methods(http,
                                   EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD);
        evhttp_set_gencb(http, GenericHandler, this);

        if (evhttp_bind_socket(http, config->GetServerIp().c_str(),
                               config->GetServerPort()) != 0)
        {
            MYLOG_FATAL(mylog::GetLogger("asynclogger"),
                        "bind %s:%u failed", config->GetServerIp().c_str(),
                        config->GetServerPort());
            evhttp_free(http);
            event_base_free(base);
            return false;
        }

        event *interrupt_event = evsignal_new(base, SIGINT, SignalHandler, base);
        event *terminate_event = evsignal_new(base, SIGTERM, SignalHandler, base);
        if (interrupt_event == nullptr || terminate_event == nullptr ||
            event_add(interrupt_event, nullptr) != 0 ||
            event_add(terminate_event, nullptr) != 0)
        {
            if (interrupt_event != nullptr)
                event_free(interrupt_event);
            if (terminate_event != nullptr)
                event_free(terminate_event);
            evhttp_free(http);
            event_base_free(base);
            return false;
        }

        MYLOG_INFO(mylog::GetLogger("asynclogger"), "server listening on %s:%u",
                   config->GetServerIp().c_str(), config->GetServerPort());
        int dispatch_result = event_base_dispatch(base);

        event_free(interrupt_event);
        event_free(terminate_event);
        evhttp_free(http);
        event_base_free(base);
        return dispatch_result >= 0;
    }

    void Service::GenericHandler(evhttp_request *request, void *context)
    {
        static_cast<Service *>(context)->HandleRequest(request);
    }

    void Service::SignalHandler(evutil_socket_t, short, void *context)
    {
        event_base_loopbreak(static_cast<event_base *>(context));
    }

    void Service::HandleRequest(evhttp_request *request)
    {
        AddSecurityHeaders(request);
        const evhttp_uri *uri = evhttp_request_get_evhttp_uri(request);
        const char *raw_path = uri == nullptr ? nullptr : evhttp_uri_get_path(uri);
        if (raw_path == nullptr)
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid path");
            return;
        }

        std::string path;
        if (!UrlDecode(raw_path, &path))
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid URL encoding");
            return;
        }

        const evhttp_cmd_type method = evhttp_request_get_command(request);
        if (path == "/login")
        {
            if (method != EVHTTP_REQ_GET)
                SendMethodNotAllowed(request, "GET");
            else
                ShowLogin(request);
            return;
        }
        if (path == "/api/login")
        {
            if (method != EVHTTP_REQ_POST)
                SendMethodNotAllowed(request, "POST");
            else
                Login(request);
            return;
        }

        AuthContext auth;
        if (!Authenticate(request, &auth))
        {
            if (path == "/" || IsDownloadPath(path))
            {
                AddHeader(request, "Location", "/login");
                evhttp_send_reply(request, HTTP_MOVETEMP, "Redirect", nullptr);
            }
            else
            {
                SendText(request, 401, "Unauthorized", "authentication required");
            }
            return;
        }

        if (path == "/")
        {
            if (method != EVHTTP_REQ_GET)
                SendMethodNotAllowed(request, "GET");
            else
                ListFiles(request);
        }
        else if (path == "/upload")
        {
            if (method != EVHTTP_REQ_POST)
                SendMethodNotAllowed(request, "POST");
            else if (!ValidateCsrf(request, auth))
                SendText(request, 403, "Forbidden", "invalid CSRF token");
            else
                Upload(request);
        }
        else if (path == "/delete")
        {
            if (method != EVHTTP_REQ_POST)
                SendMethodNotAllowed(request, "POST");
            else if (!ValidateCsrf(request, auth))
                SendText(request, 403, "Forbidden", "invalid CSRF token");
            else
                Delete(request);
        }
        else if (path == "/logout")
        {
            if (method != EVHTTP_REQ_POST)
                SendMethodNotAllowed(request, "POST");
            else if (!ValidateCsrf(request, auth))
                SendText(request, 403, "Forbidden", "invalid CSRF token");
            else
                Logout(request);
        }
        else if (IsDownloadPath(path))
        {
            if (method != EVHTTP_REQ_GET && method != EVHTTP_REQ_HEAD)
                SendMethodNotAllowed(request, "GET, HEAD");
            else
                Download(request, path);
        }
        else
        {
            SendText(request, HTTP_NOTFOUND, "Not Found", "not found");
        }
    }

    void Service::ShowLogin(evhttp_request *request)
    {
        SetNoCache(request);
        std::ifstream input("login.html", std::ios::binary);
        if (!input.is_open())
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "login template unavailable");
            return;
        }
        std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
        evbuffer_add(evhttp_request_get_output_buffer(request), content.data(),
                     content.size());
        AddHeader(request, "Content-Type", "text/html;charset=utf-8");
        evhttp_send_reply(request, HTTP_OK, "OK", nullptr);
    }

    void Service::Login(evhttp_request *request)
    {
        SetNoCache(request);
        const std::string peer = PeerAddress(request);
        if (!LoginLimiter().Allow(peer))
        {
            AddHeader(request, "Retry-After", "60");
            SendText(request, 429, "Too Many Requests", "try again later");
            return;
        }

        std::string password;
        if (!ReadRequestBody(request, 1024, &password))
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid password body");
            return;
        }

        std::string derived_hash;
        if (!DerivePasswordHash(password, &derived_hash) ||
            !SecureEqual(derived_hash, Config::GetInstance()->GetAuthHash()))
        {
            LoginLimiter().Failure(peer);
            SendText(request, 401, "Unauthorized", "authentication failed");
            return;
        }

        std::string session_token;
        std::string csrf_token;
        if (!Sessions().Create(Config::GetInstance()->GetSessionTtlSeconds(),
                               &session_token, &csrf_token))
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot create session");
            return;
        }

        LoginLimiter().Success(peer);
        AddHeader(request, "Set-Cookie",
                  "session=" + session_token + CookieSuffix(true));
        AddHeader(request, "Set-Cookie",
                  "csrf=" + csrf_token + CookieSuffix(false));
        SendText(request, HTTP_OK, "OK", "login successful");
    }

    void Service::Logout(evhttp_request *request)
    {
        AuthContext auth;
        if (Authenticate(request, &auth))
            Sessions().Remove(auth.session_token);
        ClearAuthCookies(request);
        SetNoCache(request);
        SendText(request, HTTP_OK, "OK", "logged out");
    }

    void Service::Upload(evhttp_request *request)
    {
        Config *config = Config::GetInstance();
        evbuffer *input = evhttp_request_get_input_buffer(request);
        if (input == nullptr)
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "request body missing");
            return;
        }
        size_t length = evbuffer_get_length(input);
        if (length == 0)
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "empty files are not accepted");
            return;
        }
        if (length > config->GetMaxUploadSize())
        {
            SendText(request, HTTP_ENTITYTOOLARGE, "Payload Too Large", "file too large");
            return;
        }

        const char *filename_header =
            evhttp_find_header(InputHeaders(request), "FileName");
        if (filename_header == nullptr || std::strlen(filename_header) == 0 ||
            std::strlen(filename_header) > 1024)
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid FileName header");
            return;
        }

        std::string filename;
        if (!base64_decode(filename_header, &filename) || !IsSafeFileName(filename))
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid filename");
            return;
        }

        const char *storage_type_header =
            evhttp_find_header(InputHeaders(request), "StorageType");
        if (storage_type_header == nullptr)
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "StorageType is required");
            return;
        }
        std::string storage_type = storage_type_header;
        const std::string *directory = nullptr;
        if (storage_type == "low")
            directory = &config->GetLowStorageDir();
        else if (storage_type == "deep")
            directory = &config->GetDeepStorageDir();
        else
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid storage type");
            return;
        }

        const std::string url = config->GetDownloadPrefix() + filename;
        StorageInfo existing;
        if (data_manager_.GetOneByURL(url, &existing))
        {
            SendText(request, 409, "Conflict", "a file with this name already exists");
            return;
        }
        if (!FileUtil(*directory).CreateDirectory())
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot create storage directory");
            return;
        }

        std::string content(length, '\0');
        ev_ssize_t copied = evbuffer_copyout(input, content.data(), length);
        if (copied < 0 || static_cast<size_t>(copied) != length)
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot read upload body");
            return;
        }

        std::string temporary_path;
        if (!FileUtil::CreateTempFile(*directory, ".upload-", &temporary_path))
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot create upload temporary file");
            return;
        }
        TempFileGuard temporary(temporary_path);

        bool stored = storage_type == "low"
                          ? FileUtil(temporary_path).SetContent(content.data(), content.size())
                          : FileUtil(temporary_path).Compress(content,
                                                              config->GetBundleFormat());
        if (!stored)
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot store upload");
            return;
        }

        const std::string final_path = FileUtil::JoinPath(*directory, filename);
        if (!FileUtil::CommitNoReplace(temporary_path, final_path))
        {
            int saved_errno = errno;
            if (saved_errno == EEXIST)
                SendText(request, 409, "Conflict", "a file with this name already exists");
            else
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot commit upload");
            return;
        }
        temporary.Release();

        StorageInfo info;
        if (!info.NewStorageInfo(final_path, final_path, storage_type))
        {
            std::remove(final_path.c_str());
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot read stored file metadata");
            return;
        }

        DataResult result = data_manager_.Insert(info);
        if (result != DataResult::OK)
        {
            std::remove(final_path.c_str());
            FileUtil::SyncDirectory(FileUtil::ParentPath(final_path));
            if (result == DataResult::ALREADY_EXISTS)
                SendText(request, 409, "Conflict", "a file with this name already exists");
            else
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot persist upload metadata");
            return;
        }

        SendText(request, 201, "Created", "upload successful");
    }

    void Service::Delete(evhttp_request *request)
    {
        const char *url_header =
            evhttp_find_header(InputHeaders(request), "DeleteUrl");
        if (url_header == nullptr || std::strlen(url_header) == 0 ||
            std::strlen(url_header) > 4096)
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid DeleteUrl header");
            return;
        }

        std::string url;
        if (!UrlDecode(url_header, &url) || !IsDownloadPath(url))
        {
            SendText(request, HTTP_BADREQUEST, "Bad Request", "invalid download URL");
            return;
        }

        StorageInfo info;
        if (!data_manager_.GetOneByURL(url, &info))
        {
            SendText(request, HTTP_NOTFOUND, "Not Found", "file not found");
            return;
        }

        TempFileGuard trash;
        bool moved_to_trash = false;
        std::error_code existence_error;
        const bool file_exists =
            FileUtil(info.storage_path_).Exists(&existence_error);
        if (existence_error)
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot inspect stored file");
            return;
        }
        if (file_exists)
        {
            std::string trash_path;
            const std::string parent = FileUtil::ParentPath(info.storage_path_);
            if (!FileUtil::CreateTempFile(parent, ".delete-", &trash_path))
            {
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot prepare delete transaction");
                return;
            }
            std::remove(trash_path.c_str());
            if (std::rename(info.storage_path_.c_str(), trash_path.c_str()) != 0)
            {
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot move file to delete transaction");
                return;
            }
            trash.Reset(trash_path);
            moved_to_trash = true;
        }

        DataResult result = data_manager_.Delete(url);
        if (result != DataResult::OK)
        {
            if (moved_to_trash)
            {
                if (std::rename(trash.Path().c_str(), info.storage_path_.c_str()) == 0)
                    trash.Release();
                else
                {
                    MYLOG_FATAL(mylog::GetLogger("asynclogger"),
                                "failed to restore %s from %s after metadata delete failure",
                                info.storage_path_.c_str(), trash.Path().c_str());
                    // Preserve the only remaining copy for manual recovery.
                    trash.Release();
                }
            }
            if (result == DataResult::NOT_FOUND)
                SendText(request, HTTP_NOTFOUND, "Not Found", "file not found");
            else
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot persist delete metadata");
            return;
        }

        if (moved_to_trash && std::remove(trash.Path().c_str()) == 0)
            trash.Release();
        SendText(request, HTTP_OK, "OK", "delete successful");
    }

    std::string Service::GenerateFileList(const std::vector<StorageInfo> &files) const
    {
        std::ostringstream output;
        output << "<div class='file-list'><h3>已上传文件</h3>";
        for (const StorageInfo &file : files)
        {
            const std::string filename = FileUtil(file.storage_path_).FileName();
            const std::string encoded_url = UrlEncode(file.url_, true);
            output << "<div class='file-item'>"
                   << "<div class='file-info'>"
                   << "<span>📄" << HtmlEscape(filename) << "</span>"
                   << "<span class='file-type'>"
                   << (file.storage_type_ == "deep" ? "深度存储" : "普通存储")
                   << "</span>"
                   << "<span>" << FormatSize(file.fsize_) << "</span>"
                   << "<span>" << FormatTime(file.mtime_) << "</span>"
                   << "</div>"
                   << "<div class='file-actions'>"
                   << "<button data-url=\"" << HtmlEscape(encoded_url)
                   << "\" onclick=\"window.location=this.dataset.url\">⬇️ 下载</button>"
                   << "<button data-url=\"" << HtmlEscape(encoded_url)
                   << "\" onclick=\"deleteFile(this.dataset.url)\" class='delete-btn'>🗑️ 删除</button>"
                   << "</div></div>";
        }
        output << "</div>";
        return output.str();
    }

    void Service::ListFiles(evhttp_request *request)
    {
        SetNoCache(request);
        std::vector<StorageInfo> files;
        if (!data_manager_.GetAll(&files))
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot load file list");
            return;
        }
        std::sort(files.begin(), files.end(), [](const StorageInfo &left,
                                                 const StorageInfo &right) {
            return FileUtil(left.storage_path_).FileName() <
                   FileUtil(right.storage_path_).FileName();
        });

        std::ifstream input("index.html", std::ios::binary);
        if (!input.is_open())
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "index template unavailable");
            return;
        }
        std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
        if (!ReplaceOnce(&content, "{{FILE_LIST}}", GenerateFileList(files)))
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "index template is invalid");
            return;
        }

        evbuffer_add(evhttp_request_get_output_buffer(request), content.data(),
                     content.size());
        AddHeader(request, "Content-Type", "text/html;charset=utf-8");
        evhttp_send_reply(request, HTTP_OK, "OK", nullptr);
    }

    void Service::Download(evhttp_request *request, const std::string &path)
    {
        StorageInfo info;
        if (!data_manager_.GetOneByURL(path, &info))
        {
            SendText(request, HTTP_NOTFOUND, "Not Found", "file not found");
            return;
        }

        std::string download_path = info.storage_path_;
        TempFileGuard temporary;
        if (info.storage_type_ == "deep")
        {
            std::string temporary_path;
            const std::string &directory = Config::GetInstance()->GetTempDir();
            if (!FileUtil::CreateTempFile(directory, ".download-", &temporary_path))
            {
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot create download temporary file");
                return;
            }
            temporary.Reset(temporary_path);
            if (!FileUtil(info.storage_path_).UnCompress(temporary_path))
            {
                SendText(request, HTTP_INTERNAL, "Internal Server Error",
                         "cannot decompress file");
                return;
            }
            download_path = temporary_path;
        }

        FileUtil file(download_path);
        int64_t signed_size = file.FileSize();
        if (signed_size < 0)
        {
            SendText(request, HTTP_NOTFOUND, "Not Found", "file not found");
            return;
        }
        uint64_t file_size = static_cast<uint64_t>(signed_size);
        const std::string etag = EntityTag(info);

        AddHeader(request, "Accept-Ranges", "bytes");
        AddHeader(request, "ETag", etag);
        AddHeader(request, "Content-Type", "application/octet-stream");
        const std::string filename = FileUtil(info.storage_path_).FileName();
        AddHeader(request, "Content-Disposition",
                  "attachment; filename*=UTF-8''" + UrlEncode(filename));

        ByteRange range;
        const char *range_header = evhttp_find_header(InputHeaders(request), "Range");
        const char *if_range = evhttp_find_header(InputHeaders(request), "If-Range");
        if (range_header != nullptr &&
            (if_range == nullptr || SecureEqual(if_range, etag)))
            range = ParseRangeHeader(range_header, file_size);

        if (range.status == RangeStatus::UNSATISFIABLE)
        {
            AddHeader(request, "Content-Range",
                      "bytes */" + std::to_string(file_size));
            SendText(request, 416, "Range Not Satisfiable");
            return;
        }

        uint64_t offset = range.status == RangeStatus::PARTIAL ? range.offset : 0;
        uint64_t length =
            range.status == RangeStatus::PARTIAL ? range.length : file_size;
        AddHeader(request, "Content-Length", std::to_string(length));
        if (range.status == RangeStatus::PARTIAL)
        {
            AddHeader(request, "Content-Range",
                      "bytes " + std::to_string(offset) + '-' +
                          std::to_string(offset + length - 1) + '/' +
                          std::to_string(file_size));
        }

        if (evhttp_request_get_command(request) == EVHTTP_REQ_HEAD)
        {
            evhttp_send_reply(request,
                              range.status == RangeStatus::PARTIAL ? 206 : HTTP_OK,
                              range.status == RangeStatus::PARTIAL ? "Partial Content" : "OK",
                              nullptr);
            return;
        }

        if (offset > static_cast<uint64_t>(std::numeric_limits<ev_off_t>::max()) ||
            length > static_cast<uint64_t>(std::numeric_limits<ev_off_t>::max()))
        {
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "file is too large for this server");
            return;
        }

        int fd = open(download_path.c_str(), O_RDONLY);
        if (fd == -1)
        {
            SendText(request, HTTP_NOTFOUND, "Not Found", "file not found");
            return;
        }

        evbuffer *output = evhttp_request_get_output_buffer(request);
        if (evbuffer_add_file(output, fd, static_cast<ev_off_t>(offset),
                              static_cast<ev_off_t>(length)) == -1)
        {
            close(fd);
            SendText(request, HTTP_INTERNAL, "Internal Server Error",
                     "cannot prepare file response");
            return;
        }

        if (!temporary.Path().empty())
        {
            std::remove(temporary.Path().c_str());
            temporary.Release();
        }
        evhttp_send_reply(request,
                          range.status == RangeStatus::PARTIAL ? 206 : HTTP_OK,
                          range.status == RangeStatus::PARTIAL ? "Partial Content" : "OK",
                          nullptr);
    }
} // namespace storage
