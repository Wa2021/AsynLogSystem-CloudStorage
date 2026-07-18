#pragma once

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

#include "Util.hpp"

namespace storage
{
    class Config
    {
    public:
        static Config *GetInstance()
        {
            static Config instance;
            return &instance;
        }

        uint16_t GetServerPort() const { return server_port_; }
        const std::string &GetServerIp() const { return server_ip_; }
        const std::string &GetDownloadPrefix() const { return download_prefix_; }
        int GetBundleFormat() const { return bundle_format_; }
        const std::string &GetDeepStorageDir() const { return deep_storage_dir_; }
        const std::string &GetLowStorageDir() const { return low_storage_dir_; }
        const std::string &GetTempDir() const { return temp_dir_; }
        const std::string &GetStorageInfoFile() const { return storage_info_; }
        const std::string &GetAuthSalt() const { return auth_salt_; }
        const std::string &GetAuthHash() const { return auth_hash_; }
        int GetAuthIterations() const { return auth_iterations_; }
        int GetSessionTtlSeconds() const { return session_ttl_seconds_; }
        size_t GetMaxUploadSize() const { return max_upload_size_; }
        bool GetCookieSecure() const { return cookie_secure_; }

    private:
        Config() { ReadConfig(); }

        void ReadConfig()
        {
            const char *configured_path = std::getenv("CLOUD_STORAGE_CONFIG");
            const std::string path =
                configured_path != nullptr && *configured_path != '\0'
                    ? configured_path
                    : "Storage.conf";

            std::string content;
            if (!FileUtil(path).GetContent(&content))
                throw std::runtime_error("cannot read storage config: " + path);

            Json::Value root;
            std::string error;
            if (!JsonUtil::UnSerialize(content, &root, &error) || !root.isObject())
                throw std::runtime_error("invalid storage config: " + error);

            server_port_ = ReadPort(root, "server_port");
            server_ip_ = ReadString(root, "server_ip");
            download_prefix_ = ReadString(root, "download_prefix");
            deep_storage_dir_ = ReadString(root, "deep_storage_dir");
            low_storage_dir_ = ReadString(root, "low_storage_dir");
            temp_dir_ = ReadString(root, "temp_dir");
            storage_info_ = ReadString(root, "storage_info");
            auth_salt_ = ReadString(root, "auth_salt");
            auth_hash_ = ReadString(root, "auth_hash");
            bundle_format_ = ReadPositiveInt(root, "bundle_format", 0, 5);
            auth_iterations_ =
                ReadPositiveInt(root, "auth_iterations", 10000, 10000000);
            session_ttl_seconds_ =
                ReadPositiveInt(root, "session_ttl_seconds", 60, 86400 * 30);
            max_upload_size_ = ReadSize(root, "max_upload_size", 1,
                                        static_cast<Json::UInt64>(4) * 1024 * 1024 * 1024);

            if (!root.isMember("cookie_secure") || !root["cookie_secure"].isBool())
                throw std::runtime_error("cookie_secure must be boolean");
            cookie_secure_ = root["cookie_secure"].asBool();

            const char *salt_override = std::getenv("CLOUD_STORAGE_AUTH_SALT");
            const char *hash_override = std::getenv("CLOUD_STORAGE_AUTH_HASH");
            if (salt_override != nullptr && *salt_override != '\0')
                auth_salt_ = salt_override;
            if (hash_override != nullptr && *hash_override != '\0')
                auth_hash_ = hash_override;

            if (download_prefix_.front() != '/' || download_prefix_.back() != '/')
                throw std::runtime_error("download_prefix must start and end with '/'");
            if (auth_hash_.size() != 64)
                throw std::runtime_error("auth_hash must be a 32-byte lowercase hex digest");
            for (char ch : auth_hash_)
            {
                if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                    throw std::runtime_error("auth_hash contains non-hex characters");
            }

            std::filesystem::path low =
                std::filesystem::path(low_storage_dir_).lexically_normal();
            std::filesystem::path deep =
                std::filesystem::path(deep_storage_dir_).lexically_normal();
            std::filesystem::path temporary =
                std::filesystem::path(temp_dir_).lexically_normal();
            if (low == deep || low == temporary || deep == temporary)
                throw std::runtime_error("storage and temporary directories must be different");
        }

        static std::string ReadString(const Json::Value &root, const char *name)
        {
            if (!root.isMember(name) || !root[name].isString() ||
                root[name].asString().empty())
                throw std::runtime_error(std::string(name) +
                                         " must be a non-empty string");
            return root[name].asString();
        }

        static uint16_t ReadPort(const Json::Value &root, const char *name)
        {
            if (!root.isMember(name) || !root[name].isUInt())
                throw std::runtime_error(std::string(name) + " must be an integer");
            unsigned value = root[name].asUInt();
            if (value == 0 || value > 65535)
                throw std::runtime_error(std::string(name) + " must be in 1..65535");
            return static_cast<uint16_t>(value);
        }

        static int ReadPositiveInt(const Json::Value &root, const char *name,
                                   int minimum, int maximum)
        {
            if (!root.isMember(name) || !root[name].isInt())
                throw std::runtime_error(std::string(name) + " must be an integer");
            int value = root[name].asInt();
            if (value < minimum || value > maximum)
                throw std::runtime_error(std::string(name) + " is out of range");
            return value;
        }

        static size_t ReadSize(const Json::Value &root, const char *name,
                               Json::UInt64 minimum, Json::UInt64 maximum)
        {
            if (!root.isMember(name) || !root[name].isUInt64())
                throw std::runtime_error(std::string(name) +
                                         " must be an unsigned integer");
            Json::UInt64 value = root[name].asUInt64();
            if (value < minimum || value > maximum ||
                value > static_cast<Json::UInt64>(std::numeric_limits<size_t>::max()))
                throw std::runtime_error(std::string(name) + " is out of range");
            return static_cast<size_t>(value);
        }

    private:
        uint16_t server_port_ = 0;
        std::string server_ip_;
        std::string download_prefix_;
        std::string deep_storage_dir_;
        std::string low_storage_dir_;
        std::string temp_dir_;
        std::string storage_info_;
        std::string auth_salt_;
        std::string auth_hash_;
        int bundle_format_ = 0;
        int auth_iterations_ = 0;
        int session_ttl_seconds_ = 0;
        size_t max_upload_size_ = 0;
        bool cookie_secure_ = false;
    };
} // namespace storage
