#pragma once

#include "Manager.hpp"

namespace mylog
{
    inline AsyncLogger::ptr GetLogger(const std::string &name)
    {
        return LoggerManager::GetInstance().GetLogger(name);
    }

    inline AsyncLogger::ptr DefaultLogger()
    {
        return LoggerManager::GetInstance().DefaultLogger();
    }
} // namespace mylog

#define MYLOG_DEBUG_DEFAULT(fmt, ...) \
    MYLOG_DEBUG(mylog::DefaultLogger(), fmt, ##__VA_ARGS__)
#define MYLOG_INFO_DEFAULT(fmt, ...) \
    MYLOG_INFO(mylog::DefaultLogger(), fmt, ##__VA_ARGS__)
#define MYLOG_WARN_DEFAULT(fmt, ...) \
    MYLOG_WARN(mylog::DefaultLogger(), fmt, ##__VA_ARGS__)
#define MYLOG_ERROR_DEFAULT(fmt, ...) \
    MYLOG_ERROR(mylog::DefaultLogger(), fmt, ##__VA_ARGS__)
#define MYLOG_FATAL_DEFAULT(fmt, ...) \
    MYLOG_FATAL(mylog::DefaultLogger(), fmt, ##__VA_ARGS__)
