#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "AsyncWorker.hpp"
#include "Level.hpp"
#include "LogFlush.hpp"
#include "Message.hpp"
#include "ThreadPool.hpp"
#include "backlog/CliBackupLog.hpp"

extern ThreadPool *tp;

namespace mylog
{
    class AsyncLogger
    {
    public:
        using ptr = std::shared_ptr<AsyncLogger>;

        AsyncLogger(std::string logger_name, std::vector<LogFlush::ptr> flushers,
                    AsyncType type)
            : logger_name_(std::move(logger_name)), flushers_(std::move(flushers)),
              worker_(std::make_shared<AsyncWorker>(
                  [this](Buffer &buffer) { RealFlush(buffer); }, type))
        {
        }

        AsyncLogger(const AsyncLogger &) = delete;
        AsyncLogger &operator=(const AsyncLogger &) = delete;
        virtual ~AsyncLogger() = default;

        const std::string &Name() const { return logger_name_; }

        void Log(LogLevel::value level, const char *file, size_t line,
                 const char *format, ...)
        {
            if (format == nullptr)
                return;

            va_list arguments;
            va_start(arguments, format);
            char *formatted = nullptr;
            int result = vasprintf(&formatted, format, arguments);
            va_end(arguments);
            if (result < 0 || formatted == nullptr)
            {
                std::perror("vasprintf failed");
                return;
            }

            Serialize(level, file == nullptr ? "unknown" : file, line, formatted);
            std::free(formatted);
        }

        void Stop()
        {
            if (worker_ != nullptr)
                worker_->Stop();
        }

        uint64_t RemoteBackupFailures() const
        {
            return remote_backup_failures_->load(std::memory_order_relaxed);
        }

        uint64_t RemoteBackupDrops() const
        {
            return remote_backup_drops_.load(std::memory_order_relaxed);
        }

    private:
        void Serialize(LogLevel::value level, const std::string &file, size_t line,
                       const char *payload)
        {
            LogMessage message(level, file, line, logger_name_, payload);
            std::string data = message.Format();

            if (!worker_->Push(data.data(), data.size()))
                return;

            if (level == LogLevel::value::FATAL)
            {
                if (!backup::Start(data))
                    remote_backup_failures_->fetch_add(1, std::memory_order_relaxed);
            }
            else if (level == LogLevel::value::ERROR &&
                     g_conf_data != nullptr && g_conf_data->backup_enabled)
            {
                auto failures = remote_backup_failures_;
                if (tp == nullptr || !tp->TryEnqueue([failures, data]() {
                        if (!backup::Start(data))
                            failures->fetch_add(1, std::memory_order_relaxed);
                    }))
                {
                    remote_backup_drops_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        void RealFlush(Buffer &buffer)
        {
            for (const auto &flusher : flushers_)
                flusher->Flush(buffer.Begin(), buffer.ReadableSize());
        }

    private:
        std::string logger_name_;
        std::vector<LogFlush::ptr> flushers_;
        AsyncWorker::ptr worker_;
        std::shared_ptr<std::atomic<uint64_t>> remote_backup_failures_{
            std::make_shared<std::atomic<uint64_t>>(0)};
        std::atomic<uint64_t> remote_backup_drops_{0};
    };

    class LoggerBuilder
    {
    public:
        using ptr = std::shared_ptr<LoggerBuilder>;

        void BuildLoggerName(const std::string &name) { logger_name_ = name; }
        void BuildLoggerType(AsyncType type) { async_type_ = type; }

        template <typename FlushType, typename... Args>
        void BuildLoggerFlush(Args &&...args)
        {
            flushers_.emplace_back(
                LogFlushFactory::CreateLog<FlushType>(std::forward<Args>(args)...));
        }

        AsyncLogger::ptr Build()
        {
            if (logger_name_.empty())
                throw std::invalid_argument("logger name cannot be empty");
            if (flushers_.empty())
                flushers_.emplace_back(std::make_shared<StdoutFlush>());
            return std::make_shared<AsyncLogger>(logger_name_, flushers_, async_type_);
        }

    private:
        std::string logger_name_ = "async_logger";
        std::vector<LogFlush::ptr> flushers_;
        AsyncType async_type_ = AsyncType::ASYNC_SAFE;
    };
} // namespace mylog

#define MYLOG_DEBUG(logger, fmt, ...) \
    (logger)->Log(mylog::LogLevel::value::DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MYLOG_INFO(logger, fmt, ...) \
    (logger)->Log(mylog::LogLevel::value::INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MYLOG_WARN(logger, fmt, ...) \
    (logger)->Log(mylog::LogLevel::value::WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MYLOG_ERROR(logger, fmt, ...) \
    (logger)->Log(mylog::LogLevel::value::ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define MYLOG_FATAL(logger, fmt, ...) \
    (logger)->Log(mylog::LogLevel::value::FATAL, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
