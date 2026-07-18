#pragma once

#include <ctime>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "Level.hpp"
#include "Util.hpp"

namespace mylog
{
    class LogMessage
    {
    public:
        LogMessage(LogLevel::value level, std::string file, size_t line,
                   std::string logger_name, std::string payload)
            : line_(line), created_at_(Util::Date::Now()), file_name_(std::move(file)),
              logger_name_(std::move(logger_name)), payload_(std::move(payload)),
              thread_id_(std::this_thread::get_id()), level_(level)
        {
        }

        std::string Format() const
        {
            tm local_time{};
            localtime_r(&created_at_, &local_time);
            char time_buffer[32];
            strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S",
                     &local_time);

            std::ostringstream output;
            output << '[' << time_buffer << "][" << thread_id_ << "]["
                   << LogLevel::ToString(level_) << "][" << logger_name_ << "]["
                   << file_name_ << ':' << line_ << "]\t" << payload_ << '\n';
            return output.str();
        }

    private:
        size_t line_;
        time_t created_at_;
        std::string file_name_;
        std::string logger_name_;
        std::string payload_;
        std::thread::id thread_id_;
        LogLevel::value level_;
    };
} // namespace mylog
