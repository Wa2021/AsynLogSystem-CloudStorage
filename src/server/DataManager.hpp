#pragma once

#include <jsoncpp/json/json.h>

#include <cstdint>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace storage
{
    struct StorageInfo
    {
        time_t mtime_ = 0;
        time_t atime_ = 0;
        uint64_t fsize_ = 0;
        std::string storage_path_;
        std::string url_;
        std::string storage_type_;

        bool NewStorageInfo(const std::string &actual_path,
                            const std::string &final_path,
                            const std::string &storage_type);
    };

    enum class DataResult
    {
        OK,
        NOT_FOUND,
        ALREADY_EXISTS,
        PERSIST_FAILED
    };

    class DataManager
    {
    public:
        DataManager();

        DataResult Insert(const StorageInfo &info);
        DataResult Delete(const std::string &url);
        bool GetOneByURL(const std::string &url, StorageInfo *info) const;
        bool GetAll(std::vector<StorageInfo> *items) const;

    private:
        using Table = std::unordered_map<std::string, StorageInfo>;

        bool InitLoad();
        bool Persist(const Table &table) const;
        static bool ParseStorageInfo(const Json::Value &value, StorageInfo *info);

    private:
        std::string storage_file_;
        mutable std::shared_mutex table_mutex_;
        std::mutex mutation_mutex_;
        Table table_;
    };
} // namespace storage
