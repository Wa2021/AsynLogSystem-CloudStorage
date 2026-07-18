#include "DataManager.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "Config.hpp"
#include "Util.hpp"

namespace storage
{
    bool StorageInfo::NewStorageInfo(const std::string &actual_path,
                                     const std::string &final_path,
                                     const std::string &storage_type)
    {
        if (storage_type != "low" && storage_type != "deep")
            return false;

        FileUtil actual_file(actual_path);
        int64_t size = actual_file.FileSize();
        time_t modified = actual_file.LastModifyTime();
        time_t accessed = actual_file.LastAccessTime();
        if (size < 0 || modified < 0 || accessed < 0)
            return false;

        mtime_ = modified;
        atime_ = accessed;
        fsize_ = static_cast<uint64_t>(size);
        storage_path_ = final_path;
        storage_type_ = storage_type;
        url_ = Config::GetInstance()->GetDownloadPrefix() +
               FileUtil(final_path).FileName();
        return true;
    }

    DataManager::DataManager()
        : storage_file_(Config::GetInstance()->GetStorageInfoFile())
    {
        if (!InitLoad())
            throw std::runtime_error("cannot load storage metadata");
    }

    bool DataManager::InitLoad()
    {
        FileUtil file(storage_file_);
        if (!file.Exists())
            return true;

        std::string body;
        if (!file.GetContent(&body))
            return false;

        Json::Value root;
        std::string error;
        if (!JsonUtil::UnSerialize(body, &root, &error))
            return false;
        if (root.isNull())
            return true;
        if (!root.isArray())
            return false;

        Table loaded;
        for (Json::ArrayIndex index = 0; index < root.size(); ++index)
        {
            StorageInfo info;
            if (!ParseStorageInfo(root[index], &info))
                return false;
            if (!loaded.emplace(info.url_, info).second)
                return false;
        }

        std::unique_lock<std::shared_mutex> lock(table_mutex_);
        table_.swap(loaded);
        return true;
    }

    bool DataManager::ParseStorageInfo(const Json::Value &value,
                                       StorageInfo *info)
    {
        if (info == nullptr || !value.isObject() ||
            !value["storage_path_"].isString() || !value["url_"].isString() ||
            value["storage_path_"].asString().empty() ||
            value["url_"].asString().empty() ||
            !(value["fsize_"].isUInt64() || value["fsize_"].isInt64()) ||
            !value["mtime_"].isInt64() || !value["atime_"].isInt64())
            return false;

        Json::Int64 signed_size = value["fsize_"].isInt64()
                                         ? value["fsize_"].asInt64()
                                         : 0;
        Json::Int64 modified = value["mtime_"].asInt64();
        Json::Int64 accessed = value["atime_"].asInt64();
        if ((value["fsize_"].isInt64() && signed_size < 0) || modified < 0 ||
            accessed < 0)
            return false;

        info->fsize_ = value["fsize_"].isUInt64()
                           ? value["fsize_"].asUInt64()
                           : static_cast<Json::UInt64>(signed_size);
        info->mtime_ = static_cast<time_t>(modified);
        info->atime_ = static_cast<time_t>(accessed);
        info->storage_path_ = value["storage_path_"].asString();
        info->url_ = value["url_"].asString();

        if (value.isMember("storage_type_") && value["storage_type_"].isString())
        {
            info->storage_type_ = value["storage_type_"].asString();
        }
        else
        {
            std::filesystem::path parent =
                std::filesystem::path(info->storage_path_).parent_path().lexically_normal();
            std::filesystem::path deep =
                std::filesystem::path(Config::GetInstance()->GetDeepStorageDir())
                    .lexically_normal();
            info->storage_type_ = parent == deep ? "deep" : "low";
        }

        if (info->storage_type_ != "low" && info->storage_type_ != "deep")
            return false;

        const std::filesystem::path storage_path =
            std::filesystem::path(info->storage_path_).lexically_normal();
        const std::filesystem::path expected_parent =
            std::filesystem::path(
                info->storage_type_ == "deep"
                    ? Config::GetInstance()->GetDeepStorageDir()
                    : Config::GetInstance()->GetLowStorageDir())
                .lexically_normal();
        if (storage_path.filename().empty() ||
            storage_path.parent_path() != expected_parent)
            return false;

        return info->url_ == Config::GetInstance()->GetDownloadPrefix() +
                                 storage_path.filename().string();
    }

    bool DataManager::Persist(const Table &table) const
    {
        Json::Value root(Json::arrayValue);
        for (const auto &entry : table)
        {
            const StorageInfo &info = entry.second;
            Json::Value item;
            item["mtime_"] = static_cast<Json::Int64>(info.mtime_);
            item["atime_"] = static_cast<Json::Int64>(info.atime_);
            item["fsize_"] = static_cast<Json::UInt64>(info.fsize_);
            item["url_"] = info.url_;
            item["storage_path_"] = info.storage_path_;
            item["storage_type_"] = info.storage_type_;
            root.append(item);
        }

        std::string body;
        if (!JsonUtil::Serialize(root, &body))
            return false;

        const std::string parent = FileUtil::ParentPath(storage_file_);
        if (!FileUtil(parent).CreateDirectory())
            return false;

        std::string temporary_path;
        if (!FileUtil::CreateTempFile(parent, ".storage-", &temporary_path))
            return false;

        if (!FileUtil(temporary_path).SetContent(body.data(), body.size()))
        {
            std::remove(temporary_path.c_str());
            return false;
        }
        if (std::rename(temporary_path.c_str(), storage_file_.c_str()) != 0)
        {
            std::remove(temporary_path.c_str());
            return false;
        }

        // rename already committed the snapshot. Directory fsync only strengthens
        // crash durability and must not make memory lag behind the disk state.
        (void)FileUtil::SyncDirectory(parent);
        return true;
    }

    DataResult DataManager::Insert(const StorageInfo &info)
    {
        std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
        Table next;
        {
            std::shared_lock<std::shared_mutex> table_lock(table_mutex_);
            next = table_;
        }
        if (next.find(info.url_) != next.end())
            return DataResult::ALREADY_EXISTS;

        next.emplace(info.url_, info);
        if (!Persist(next))
            return DataResult::PERSIST_FAILED;

        {
            std::unique_lock<std::shared_mutex> table_lock(table_mutex_);
            table_.swap(next);
        }
        return DataResult::OK;
    }

    DataResult DataManager::Delete(const std::string &url)
    {
        std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
        Table next;
        {
            std::shared_lock<std::shared_mutex> table_lock(table_mutex_);
            next = table_;
        }
        if (next.erase(url) == 0)
            return DataResult::NOT_FOUND;
        if (!Persist(next))
            return DataResult::PERSIST_FAILED;

        {
            std::unique_lock<std::shared_mutex> table_lock(table_mutex_);
            table_.swap(next);
        }
        return DataResult::OK;
    }

    bool DataManager::GetOneByURL(const std::string &url, StorageInfo *info) const
    {
        if (info == nullptr)
            return false;
        std::shared_lock<std::shared_mutex> lock(table_mutex_);
        auto iterator = table_.find(url);
        if (iterator == table_.end())
            return false;
        *info = iterator->second;
        return true;
    }

    bool DataManager::GetAll(std::vector<StorageInfo> *items) const
    {
        if (items == nullptr)
            return false;
        std::shared_lock<std::shared_mutex> lock(table_mutex_);
        items->clear();
        items->reserve(table_.size());
        for (const auto &entry : table_)
            items->push_back(entry.second);
        return true;
    }
} // namespace storage
