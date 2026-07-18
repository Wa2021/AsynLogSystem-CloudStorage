#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AsyncLogger.hpp"

namespace mylog
{
    class LoggerManager
    {
    public:
        static LoggerManager &GetInstance()
        {
            static LoggerManager instance;
            return instance;
        }

        bool AddLogger(const AsyncLogger::ptr &logger)
        {
            if (logger == nullptr)
                return false;
            std::lock_guard<std::mutex> lock(mutex_);
            return loggers_.emplace(logger->Name(), logger).second;
        }

        AsyncLogger::ptr GetLogger(const std::string &name)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto iterator = loggers_.find(name);
            return iterator == loggers_.end() ? default_logger_ : iterator->second;
        }

        AsyncLogger::ptr DefaultLogger()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return default_logger_;
        }

        void Shutdown()
        {
            std::unordered_map<std::string, AsyncLogger::ptr> loggers;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                loggers.swap(loggers_);
                default_logger_.reset();
            }
            for (auto &entry : loggers)
                entry.second->Stop();
        }

    private:
        LoggerManager()
        {
            LoggerBuilder builder;
            builder.BuildLoggerName("default");
            default_logger_ = builder.Build();
            loggers_.emplace(default_logger_->Name(), default_logger_);
        }

    private:
        std::mutex mutex_;
        AsyncLogger::ptr default_logger_;
        std::unordered_map<std::string, AsyncLogger::ptr> loggers_;
    };
} // namespace mylog
