#pragma once

#include <jsoncpp/json/json.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../../log_system/logs_code/MyLog.hpp"
#include <bundle.h>

namespace storage
{
    namespace fs = std::filesystem;

    inline bool FromHex(unsigned char input, unsigned char *output)
    {
        if (output == nullptr)
            return false;
        if (input >= 'A' && input <= 'F')
            *output = input - 'A' + 10;
        else if (input >= 'a' && input <= 'f')
            *output = input - 'a' + 10;
        else if (input >= '0' && input <= '9')
            *output = input - '0';
        else
            return false;
        return true;
    }

    inline bool UrlDecode(const std::string &input, std::string *output)
    {
        if (output == nullptr)
            return false;
        output->clear();
        output->reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] != '%')
            {
                output->push_back(input[i]);
                continue;
            }

            if (i + 2 >= input.size())
                return false;
            unsigned char high = 0;
            unsigned char low = 0;
            if (!FromHex(static_cast<unsigned char>(input[i + 1]), &high) ||
                !FromHex(static_cast<unsigned char>(input[i + 2]), &low))
                return false;

            char decoded = static_cast<char>(high * 16 + low);
            if (decoded == '\0')
                return false;
            output->push_back(decoded);
            i += 2;
        }
        return true;
    }

    inline std::string UrlEncode(const std::string &input, bool preserve_slash = false)
    {
        static const char hex[] = "0123456789ABCDEF";
        std::string output;
        output.reserve(input.size());
        for (unsigned char ch : input)
        {
            bool unreserved = (ch >= 'A' && ch <= 'Z') ||
                              (ch >= 'a' && ch <= 'z') ||
                              (ch >= '0' && ch <= '9') || ch == '-' ||
                              ch == '_' || ch == '.' || ch == '~';
            if (unreserved || (preserve_slash && ch == '/'))
            {
                output.push_back(static_cast<char>(ch));
            }
            else
            {
                output.push_back('%');
                output.push_back(hex[ch >> 4]);
                output.push_back(hex[ch & 0x0f]);
            }
        }
        return output;
    }

    class FileUtil
    {
    public:
        explicit FileUtil(std::string filename) : filename_(std::move(filename)) {}

        int64_t FileSize() const
        {
            struct stat status{};
            if (stat(filename_.c_str(), &status) == -1)
                return -1;
            return status.st_size;
        }

        time_t LastAccessTime() const
        {
            struct stat status{};
            return stat(filename_.c_str(), &status) == -1 ? -1 : status.st_atime;
        }

        time_t LastModifyTime() const
        {
            struct stat status{};
            return stat(filename_.c_str(), &status) == -1 ? -1 : status.st_mtime;
        }

        std::string FileName() const
        {
            return fs::path(filename_).filename().string();
        }

        bool GetPosLen(std::string *content, size_t position, size_t length) const
        {
            if (content == nullptr)
                return false;
            int64_t file_size = FileSize();
            if (file_size < 0 || position > static_cast<uint64_t>(file_size) ||
                length > static_cast<uint64_t>(file_size) - position ||
                position > static_cast<size_t>(std::numeric_limits<off_t>::max()))
                return false;

            int fd = open(filename_.c_str(), O_RDONLY);
            if (fd == -1)
                return false;

            content->assign(length, '\0');
            size_t total = 0;
            while (total < length)
            {
                ssize_t count = pread(fd, content->data() + total, length - total,
                                      static_cast<off_t>(position + total));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    close(fd);
                    return false;
                }
                total += static_cast<size_t>(count);
            }
            return close(fd) == 0;
        }

        bool GetContent(std::string *content) const
        {
            int64_t file_size = FileSize();
            if (file_size < 0 ||
                static_cast<uint64_t>(file_size) >
                    static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
                return false;
            return GetPosLen(content, 0, static_cast<size_t>(file_size));
        }

        bool SetContent(const char *content, size_t length) const
        {
            if (content == nullptr && length != 0)
                return false;
            int fd = open(filename_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1)
                return false;

            size_t total = 0;
            while (total < length)
            {
                ssize_t count = write(fd, content + total, length - total);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                {
                    close(fd);
                    return false;
                }
                total += static_cast<size_t>(count);
            }

            bool ok = fsync(fd) == 0;
            if (close(fd) == -1)
                ok = false;
            return ok;
        }

        bool Compress(const std::string &content, int format) const
        {
            try
            {
                std::string packed = bundle::pack(format, content);
                return SetContent(packed.data(), packed.size());
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        bool UnCompress(const std::string &output_path) const
        {
            try
            {
                std::string packed;
                if (!GetContent(&packed))
                    return false;
                std::string unpacked = bundle::unpack(packed);
                return FileUtil(output_path).SetContent(unpacked.data(), unpacked.size());
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        bool Exists() const
        {
            std::error_code error;
            return Exists(&error);
        }

        bool Exists(std::error_code *error) const
        {
            if (error == nullptr)
                return false;
            error->clear();
            bool exists = fs::exists(filename_, *error);
            return exists && !*error;
        }

        bool CreateDirectory() const
        {
            std::error_code error;
            if (fs::exists(filename_, error))
                return !error && fs::is_directory(filename_, error);
            return fs::create_directories(filename_, error) && !error;
        }

        bool ScanDirectory(std::vector<std::string> *files) const
        {
            if (files == nullptr)
                return false;
            std::error_code error;
            fs::directory_iterator iterator(filename_, error);
            if (error)
                return false;
            for (const auto &entry : iterator)
            {
                if (!entry.is_directory(error))
                    files->push_back(entry.path().string());
                if (error)
                    return false;
            }
            return true;
        }

        static std::string JoinPath(const std::string &directory,
                                    const std::string &filename)
        {
            return (fs::path(directory) / filename).string();
        }

        static std::string ParentPath(const std::string &path)
        {
            fs::path parent = fs::path(path).parent_path();
            return parent.empty() ? "." : parent.string();
        }

        static bool CreateTempFile(const std::string &directory,
                                   const std::string &prefix,
                                   std::string *path)
        {
            if (path == nullptr || prefix.find('/') != std::string::npos)
                return false;
            if (!FileUtil(directory).CreateDirectory())
                return false;

            std::string pattern = JoinPath(directory, prefix + "XXXXXX");
            std::vector<char> buffer(pattern.begin(), pattern.end());
            buffer.push_back('\0');
            int fd = mkstemp(buffer.data());
            if (fd == -1)
                return false;
            if (close(fd) == -1)
            {
                unlink(buffer.data());
                return false;
            }
            *path = buffer.data();
            return true;
        }

        static bool CommitNoReplace(const std::string &temporary_path,
                                    const std::string &final_path)
        {
            if (link(temporary_path.c_str(), final_path.c_str()) == -1)
                return false;
            if (unlink(temporary_path.c_str()) == -1)
            {
                int saved_errno = errno;
                unlink(final_path.c_str());
                errno = saved_errno;
                return false;
            }
            // The hard link and temporary-name removal already committed the file.
            // Directory fsync only strengthens crash durability and is best effort.
            (void)SyncDirectory(ParentPath(final_path));
            return true;
        }

        static bool SyncDirectory(const std::string &directory)
        {
            int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
            if (fd == -1)
                return false;
            bool ok = fsync(fd) == 0;
            if (close(fd) == -1)
                ok = false;
            return ok;
        }

    private:
        std::string filename_;
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
} // namespace storage
