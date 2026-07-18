#include <jsoncpp/json/json.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "../DataManager.hpp"
#include "../Range.hpp"
#include "../Util.hpp"
#include "../base64.h"
#include "../../../log_system/logs_code/AsyncWorker.hpp"
#include "../../../log_system/logs_code/ThreadPool.hpp"

ThreadPool *tp = nullptr;
mylog::Util::JsonData *g_conf_data = nullptr;

namespace
{
    namespace fs = std::filesystem;

    class TestDirectory
    {
    public:
        TestDirectory()
        {
            char pattern[] = "/tmp/asynlog-tests-XXXXXX";
            char *created = mkdtemp(pattern);
            if (created == nullptr)
                throw std::runtime_error("mkdtemp failed");
            path_ = created;
        }

        ~TestDirectory()
        {
            std::error_code error;
            fs::remove_all(path_, error);
        }

        const fs::path &Path() const { return path_; }

    private:
        fs::path path_;
    };

    void Check(bool condition, const char *expression, int line)
    {
        if (!condition)
            throw std::runtime_error(std::string("check failed at line ") +
                                     std::to_string(line) + ": " + expression);
    }

#define CHECK(expression) Check((expression), #expression, __LINE__)

    void WriteJson(const fs::path &path, const Json::Value &value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            throw std::runtime_error("cannot create test configuration");
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
        if (writer->write(value, &output) != 0 || !output.good())
            throw std::runtime_error("cannot write test configuration");
    }

    void PrepareConfiguration(const fs::path &root)
    {
        Json::Value storage;
        storage["server_port"] = 18081;
        storage["server_ip"] = "127.0.0.1";
        storage["download_prefix"] = "/download/";
        storage["deep_storage_dir"] = (root / "deep").string();
        storage["low_storage_dir"] = (root / "low").string();
        storage["temp_dir"] = (root / "tmp").string();
        storage["bundle_format"] = 4;
        storage["max_upload_size"] = Json::UInt64(1024 * 1024);
        storage["storage_info"] = (root / "storage.data").string();
        storage["auth_salt"] = "test-salt";
        storage["auth_iterations"] = 10000;
        storage["auth_hash"] = std::string(64, '0');
        storage["session_ttl_seconds"] = 3600;
        storage["cookie_secure"] = false;
        const fs::path storage_config = root / "Storage.conf";
        WriteJson(storage_config, storage);

        Json::Value logging;
        logging["buffer_size"] = Json::UInt64(16);
        logging["threshold"] = Json::UInt64(32);
        logging["linear_growth"] = Json::UInt64(16);
        logging["flush_log"] = Json::UInt64(0);
        logging["backup_enabled"] = false;
        logging["backup_addr"] = "127.0.0.1";
        logging["backup_port"] = 18080;
        logging["backup_token"] = "";
        logging["backup_connect_timeout_ms"] = 50;
        logging["backup_send_timeout_ms"] = 50;
        logging["backup_retries"] = 1;
        logging["backup_queue_size"] = Json::UInt64(8);
        logging["thread_count"] = Json::UInt64(1);
        const fs::path log_config = root / "log.conf";
        WriteJson(log_config, logging);

        if (setenv("CLOUD_STORAGE_CONFIG", storage_config.c_str(), 1) != 0 ||
            setenv("ASYNLOG_CONFIG", log_config.c_str(), 1) != 0)
            throw std::runtime_error("setenv failed");
    }

    void TestUrlEncoding()
    {
        const std::string original = "folder/a b%?#";
        const std::string encoded = storage::UrlEncode(original, true);
        CHECK(encoded == "folder/a%20b%25%3F%23");

        std::string decoded;
        CHECK(storage::UrlDecode(encoded, &decoded));
        CHECK(decoded == original);
        CHECK(storage::UrlDecode("a+b", &decoded));
        CHECK(decoded == "a+b");
        CHECK(!storage::UrlDecode("%", &decoded));
        CHECK(!storage::UrlDecode("%G1", &decoded));
        CHECK(!storage::UrlDecode("%00", &decoded));
    }

    void TestBase64()
    {
        std::string decoded;
        CHECK(base64_decode(base64_encode(std::string("hello")), &decoded));
        CHECK(decoded == "hello");
        CHECK(base64_decode("SGVsbG8", &decoded));
        CHECK(decoded == "Hello");
        CHECK(!base64_decode("A", &decoded));
        CHECK(!base64_decode("AA=A", &decoded));
        CHECK(!base64_decode("AA===", &decoded));
        CHECK(!base64_decode("AA==AA==", &decoded));
        CHECK(!base64_decode("!!!!", &decoded));
    }

    void TestRanges()
    {
        storage::ByteRange range = storage::ParseRangeHeader("bytes=10-19", 100);
        CHECK(range.status == storage::RangeStatus::PARTIAL);
        CHECK(range.offset == 10 && range.length == 10);

        range = storage::ParseRangeHeader("bytes=90-", 100);
        CHECK(range.status == storage::RangeStatus::PARTIAL);
        CHECK(range.offset == 90 && range.length == 10);

        range = storage::ParseRangeHeader("bytes=-25", 100);
        CHECK(range.status == storage::RangeStatus::PARTIAL);
        CHECK(range.offset == 75 && range.length == 25);

        range = storage::ParseRangeHeader("bytes=90-200", 100);
        CHECK(range.status == storage::RangeStatus::PARTIAL);
        CHECK(range.offset == 90 && range.length == 10);

        CHECK(storage::ParseRangeHeader("bytes=100-", 100).status ==
              storage::RangeStatus::UNSATISFIABLE);
        CHECK(storage::ParseRangeHeader("bytes=20-10", 100).status ==
              storage::RangeStatus::UNSATISFIABLE);
        CHECK(storage::ParseRangeHeader("bytes=-0", 100).status ==
              storage::RangeStatus::UNSATISFIABLE);
        CHECK(storage::ParseRangeHeader("bytes=abc-def", 100).status ==
              storage::RangeStatus::IGNORE);
        CHECK(storage::ParseRangeHeader("items=0-1", 100).status ==
              storage::RangeStatus::IGNORE);
        CHECK(storage::ParseRangeHeader("bytes=0-1,4-5", 100).status ==
              storage::RangeStatus::IGNORE);
    }

    void TestFileCommit(const fs::path &root)
    {
        const fs::path directory = root / "commit";
        CHECK(storage::FileUtil(directory.string()).CreateDirectory());

        std::string temporary;
        CHECK(storage::FileUtil::CreateTempFile(directory.string(), ".new-",
                                                &temporary));
        CHECK(storage::FileUtil(temporary).SetContent("new", 3));
        const fs::path final_path = directory / "file.txt";
        CHECK(storage::FileUtil::CommitNoReplace(temporary, final_path.string()));
        CHECK(!storage::FileUtil(temporary).Exists());

        std::string content;
        CHECK(storage::FileUtil(final_path.string()).GetContent(&content));
        CHECK(content == "new");

        CHECK(storage::FileUtil::CreateTempFile(directory.string(), ".new-",
                                                &temporary));
        CHECK(storage::FileUtil(temporary).SetContent("other", 5));
        errno = 0;
        CHECK(!storage::FileUtil::CommitNoReplace(temporary, final_path.string()));
        CHECK(errno == EEXIST);
        CHECK(storage::FileUtil(final_path.string()).GetContent(&content));
        CHECK(content == "new");
        fs::remove(temporary);
    }

    void TestDataManager(const fs::path &root)
    {
        const fs::path file_path = root / "low" / "sample.txt";
        CHECK(storage::FileUtil((root / "low").string()).CreateDirectory());
        CHECK(storage::FileUtil(file_path.string()).SetContent("payload", 7));

        storage::StorageInfo info;
        CHECK(info.NewStorageInfo(file_path.string(), file_path.string(), "low"));

        storage::DataManager first;
        CHECK(first.Insert(info) == storage::DataResult::OK);
        CHECK(first.Insert(info) == storage::DataResult::ALREADY_EXISTS);

        storage::StorageInfo loaded;
        CHECK(first.GetOneByURL(info.url_, &loaded));
        CHECK(loaded.storage_path_ == info.storage_path_);
        CHECK(loaded.storage_type_ == "low");

        storage::DataManager reloaded;
        CHECK(reloaded.GetOneByURL(info.url_, &loaded));
        CHECK(reloaded.Delete(info.url_) == storage::DataResult::OK);
        CHECK(reloaded.Delete(info.url_) == storage::DataResult::NOT_FOUND);

        storage::DataManager after_delete;
        CHECK(!after_delete.GetOneByURL(info.url_, &loaded));

        Json::Value invalid_root(Json::arrayValue);
        Json::Value invalid_item;
        invalid_item["mtime_"] = Json::Int64(1);
        invalid_item["atime_"] = Json::Int64(1);
        invalid_item["fsize_"] = Json::UInt64(1);
        invalid_item["url_"] = "/download/outside.txt";
        invalid_item["storage_path_"] = (root / "outside.txt").string();
        invalid_item["storage_type_"] = "low";
        invalid_root.append(invalid_item);
        WriteJson(root / "storage.data", invalid_root);

        bool rejected_outside_path = false;
        try
        {
            storage::DataManager invalid;
        }
        catch (const std::runtime_error &)
        {
            rejected_outside_path = true;
        }
        CHECK(rejected_outside_path);

        CHECK(storage::FileUtil((root / "storage.data").string())
                  .SetContent("{", 1));
        bool rejected_corrupt_metadata = false;
        try
        {
            storage::DataManager invalid;
        }
        catch (const std::runtime_error &)
        {
            rejected_corrupt_metadata = true;
        }
        CHECK(rejected_corrupt_metadata);
    }

    void TestAsyncWorker()
    {
        g_conf_data = mylog::Util::JsonData::GetJsonData();

        const std::string large_message(4096, 'x');
        mylog::Buffer buffer;
        buffer.Push(large_message.data(), large_message.size());
        CHECK(buffer.Capacity() >= large_message.size());
        CHECK(std::string(buffer.Begin(), buffer.ReadableSize()) == large_message);

        std::string flushed;
        mylog::AsyncWorker worker([&flushed](mylog::Buffer &batch) {
            flushed.append(batch.Begin(), batch.ReadableSize());
        });
        CHECK(worker.Push(large_message.data(), large_message.size()));
        CHECK(worker.Push("tail", 4));
        worker.Stop();
        CHECK(flushed == large_message + "tail");
        CHECK(!worker.Push("late", 4));

        bool rejected_empty_pool = false;
        try
        {
            ThreadPool invalid(0);
        }
        catch (const std::invalid_argument &)
        {
            rejected_empty_pool = true;
        }
        CHECK(rejected_empty_pool);
    }

    void Run(const char *name, const std::function<void()> &test)
    {
        test();
        std::cout << "[PASS] " << name << '\n';
    }
} // namespace

int main()
{
    try
    {
        TestDirectory directory;
        PrepareConfiguration(directory.Path());

        Run("URL encoding", TestUrlEncoding);
        Run("Base64 validation", TestBase64);
        Run("Range parsing", TestRanges);
        Run("atomic file commit", [&directory]() {
            TestFileCommit(directory.Path());
        });
        Run("DataManager persistence", [&directory]() {
            TestDataManager(directory.Path());
        });
        Run("async buffer and drain", TestAsyncWorker);
        std::cout << "All tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
