#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "Util.hpp"

extern mylog::Util::JsonData *g_conf_data;

namespace mylog
{
    class LogFlush
    {
    public:
        using ptr = std::shared_ptr<LogFlush>;
        virtual ~LogFlush() = default;
        virtual void Flush(const char *data, size_t len) = 0;
    };

    class StdoutFlush : public LogFlush
    {
    public:
        void Flush(const char *data, size_t len) override
        {
            std::cout.write(data, static_cast<std::streamsize>(len));
        }
    };

    class FileFlush : public LogFlush
    {
    public:
        explicit FileFlush(const std::string &filename)
        {
            if (!Util::File::CreateDirectory(Util::File::Path(filename)))
                throw std::runtime_error("cannot create log directory");
            file_ = fopen(filename.c_str(), "ab");
            if (file_ == nullptr)
                throw std::runtime_error("cannot open log file: " + filename);
        }

        ~FileFlush() override
        {
            if (file_ != nullptr)
                fclose(file_);
        }

        void Flush(const char *data, size_t len) override
        {
            if (!WriteAll(file_, data, len))
                std::perror("write log file failed");
            ApplyFlushPolicy(file_);
        }

    public:
        static bool WriteAll(FILE *file, const char *data, size_t len)
        {
            if (file == nullptr || (data == nullptr && len != 0))
                return false;
            size_t written = 0;
            while (written < len)
            {
                size_t count = fwrite(data + written, 1, len - written, file);
                if (count == 0)
                    return false;
                written += count;
            }
            return ferror(file) == 0;
        }

        static bool ApplyFlushPolicy(FILE *file)
        {
            if (file == nullptr || g_conf_data == nullptr)
                return false;
            if (g_conf_data->flush_log >= 1 && fflush(file) == EOF)
            {
                std::perror("fflush log file failed");
                return false;
            }
            if (g_conf_data->flush_log == 2 && fsync(fileno(file)) == -1)
            {
                std::perror("fsync log file failed");
                return false;
            }
            return true;
        }

    private:
        FILE *file_ = nullptr;
    };

    class RollFileFlush : public LogFlush
    {
    public:
        RollFileFlush(const std::string &basename, size_t max_size)
            : max_size_(max_size), basename_(basename)
        {
            if (max_size_ == 0)
                throw std::invalid_argument("rolling log size must be positive");
            if (!Util::File::CreateDirectory(Util::File::Path(basename_)))
                throw std::runtime_error("cannot create rolling log directory");
        }

        ~RollFileFlush() override
        {
            if (file_ != nullptr)
                fclose(file_);
        }

        void Flush(const char *data, size_t len) override
        {
            if (!EnsureFile(len))
                return;
            if (!FileFlush::WriteAll(file_, data, len))
            {
                std::perror("write rolling log file failed");
                return;
            }
            current_size_ += len;
            FileFlush::ApplyFlushPolicy(file_);
        }

    private:
        bool EnsureFile(size_t incoming_size)
        {
            if (file_ != nullptr &&
                (current_size_ == 0 || incoming_size <= max_size_ -
                                                        std::min(current_size_, max_size_)))
                return true;

            if (file_ != nullptr)
            {
                fclose(file_);
                file_ = nullptr;
            }

            const std::string filename = CreateFilename();
            file_ = fopen(filename.c_str(), "ab");
            if (file_ == nullptr)
            {
                std::perror("open rolling log file failed");
                return false;
            }

            std::error_code ec;
            current_size_ = static_cast<size_t>(
                std::filesystem::file_size(filename, ec));
            if (ec)
                current_size_ = 0;
            return true;
        }

        std::string CreateFilename()
        {
            const auto now = std::chrono::system_clock::now();
            const time_t seconds = std::chrono::system_clock::to_time_t(now);
            tm local_time{};
            localtime_r(&seconds, &local_time);

            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time);
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                    now.time_since_epoch())
                                    .count() %
                                1000000;

            std::ostringstream filename;
            filename << basename_ << '-' << timestamp << '-' << std::setw(6)
                     << std::setfill('0') << micros << '-' << getpid() << '-'
                     << sequence_++ << ".log";
            return filename.str();
        }

    private:
        size_t max_size_;
        std::string basename_;
        size_t current_size_ = 0;
        size_t sequence_ = 1;
        FILE *file_ = nullptr;
    };

    class LogFlushFactory
    {
    public:
        template <typename FlushType, typename... Args>
        static std::shared_ptr<LogFlush> CreateLog(Args &&...args)
        {
            return std::make_shared<FlushType>(std::forward<Args>(args)...);
        }
    };
} // namespace mylog
