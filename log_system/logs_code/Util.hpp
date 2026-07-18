#pragma once

#include <jsoncpp/json/json.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mylog
{
    namespace Util
    {
        class Date
        {
        public:
            static time_t Now() { return time(nullptr); }
        };

        class File
        {
        public:
            static bool Exists(const std::string &filename)
            {
                std::error_code ec;
                return std::filesystem::exists(filename, ec) && !ec;
            }

            static std::string Path(const std::string &filename)
            {
                return std::filesystem::path(filename).parent_path().string();
            }

            static bool CreateDirectory(const std::string &pathname)
            {
                if (pathname.empty())
                    return true;
                std::error_code ec;
                if (std::filesystem::exists(pathname, ec))
                    return !ec && std::filesystem::is_directory(pathname, ec);
                return std::filesystem::create_directories(pathname, ec) && !ec;
            }

            static bool GetContent(std::string *content, const std::string &filename)
            {
                if (content == nullptr)
                    return false;

                std::ifstream input(filename, std::ios::binary | std::ios::ate);
                if (!input.is_open())
                    return false;

                std::streamoff end = input.tellg();
                if (end < 0 || static_cast<uint64_t>(end) >
                                   static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
                    return false;

                content->assign(static_cast<size_t>(end), '\0');
                input.seekg(0, std::ios::beg);
                if (!content->empty())
                {
                    input.read(content->data(), static_cast<std::streamsize>(content->size()));
                    if (input.gcount() != static_cast<std::streamsize>(content->size()))
                        return false;
                }
                return true;
            }
        };

        class JsonUtil
        {
        public:
            static bool Serialize(const Json::Value &value, std::string *output)
            {
                if (output == nullptr)
                    return false;
                Json::StreamWriterBuilder builder;
                builder["emitUTF8"] = true;
                std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
                std::stringstream stream;
                if (writer->write(value, &stream) != 0)
                    return false;
                *output = stream.str();
                return true;
            }

            static bool UnSerialize(const std::string &input, Json::Value *value,
                                    std::string *error = nullptr)
            {
                if (value == nullptr)
                    return false;
                Json::CharReaderBuilder builder;
                std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                std::string local_error;
                bool ok = reader->parse(input.data(), input.data() + input.size(),
                                        value, &local_error);
                if (!ok && error != nullptr)
                    *error = local_error;
                return ok;
            }
        };

        struct JsonData
        {
            static JsonData *GetJsonData()
            {
                static JsonData config;
                return &config;
            }

            size_t buffer_size = 0;
            size_t threshold = 0;
            size_t linear_growth = 0;
            size_t flush_log = 0;
            std::string backup_addr;
            uint16_t backup_port = 0;
            size_t thread_count = 0;
            size_t backup_queue_size = 0;
            int backup_connect_timeout_ms = 0;
            int backup_send_timeout_ms = 0;
            int backup_retries = 0;
            bool backup_enabled = false;
            std::string backup_token;

        private:
            JsonData()
            {
                const std::string path = FindConfigPath();
                std::string content;
                if (!File::GetContent(&content, path))
                    throw std::runtime_error("cannot read log config: " + path);

                Json::Value root;
                std::string error;
                if (!JsonUtil::UnSerialize(content, &root, &error) || !root.isObject())
                    throw std::runtime_error("invalid log config: " + error);

                RequireUnsigned(root, "buffer_size", &buffer_size);
                RequireUnsigned(root, "threshold", &threshold);
                RequireUnsigned(root, "linear_growth", &linear_growth);
                RequireUnsigned(root, "flush_log", &flush_log);
                RequireUnsigned(root, "thread_count", &thread_count);
                RequireUnsigned(root, "backup_queue_size", &backup_queue_size);

                if (!root["backup_addr"].isString() || root["backup_addr"].asString().empty())
                    throw std::runtime_error("backup_addr must be a non-empty string");
                backup_addr = root["backup_addr"].asString();

                if (!root["backup_port"].isUInt() || root["backup_port"].asUInt() == 0 ||
                    root["backup_port"].asUInt() > 65535)
                    throw std::runtime_error("backup_port must be in 1..65535");
                backup_port = static_cast<uint16_t>(root["backup_port"].asUInt());

                if (!root["backup_connect_timeout_ms"].isInt() ||
                    root["backup_connect_timeout_ms"].asInt() <= 0)
                    throw std::runtime_error("backup_connect_timeout_ms must be positive");
                backup_connect_timeout_ms = root["backup_connect_timeout_ms"].asInt();

                if (!root["backup_send_timeout_ms"].isInt() ||
                    root["backup_send_timeout_ms"].asInt() <= 0)
                    throw std::runtime_error("backup_send_timeout_ms must be positive");
                backup_send_timeout_ms = root["backup_send_timeout_ms"].asInt();

                if (!root["backup_retries"].isInt() || root["backup_retries"].asInt() <= 0)
                    throw std::runtime_error("backup_retries must be positive");
                backup_retries = root["backup_retries"].asInt();

                if (!root["backup_enabled"].isBool())
                    throw std::runtime_error("backup_enabled must be boolean");
                backup_enabled = root["backup_enabled"].asBool();
                backup_token = root.get("backup_token", "").asString();

                if (buffer_size == 0 || linear_growth == 0 || thread_count == 0 ||
                    backup_queue_size == 0)
                    throw std::runtime_error("log buffer, thread and queue sizes must be positive");
                if (threshold < buffer_size)
                    throw std::runtime_error("threshold must be greater than or equal to buffer_size");
                if (flush_log > 2)
                    throw std::runtime_error("flush_log must be 0, 1 or 2");
            }

            static std::string FindConfigPath()
            {
                const char *env = std::getenv("ASYNLOG_CONFIG");
                if (env != nullptr && *env != '\0')
                    return env;

                const char *candidates[] = {
                    "../../log_system/logs_code/config.conf",
                    "../logs_code/config.conf",
                    "log_system/logs_code/config.conf",
                    "config.conf"
                };
                for (const char *candidate : candidates)
                {
                    if (File::Exists(candidate))
                        return candidate;
                }
                return candidates[0];
            }

            static void RequireUnsigned(const Json::Value &root, const char *name,
                                        size_t *output)
            {
                if (!root.isMember(name) || !root[name].isUInt64())
                    throw std::runtime_error(std::string(name) + " must be an unsigned integer");
                Json::UInt64 value = root[name].asUInt64();
                if (value > static_cast<Json::UInt64>(std::numeric_limits<size_t>::max()))
                    throw std::runtime_error(std::string(name) + " is too large");
                *output = static_cast<size_t>(value);
            }
        };
    } // namespace Util
} // namespace mylog
