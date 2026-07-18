#pragma once

#include <charconv>
#include <cstdint>
#include <string>

namespace storage
{
    enum class RangeStatus
    {
        IGNORE,
        PARTIAL,
        UNSATISFIABLE
    };

    struct ByteRange
    {
        RangeStatus status = RangeStatus::IGNORE;
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    inline bool ParseUnsigned(const std::string &text, uint64_t *value)
    {
        if (value == nullptr || text.empty())
            return false;
        const char *begin = text.data();
        const char *end = begin + text.size();
        auto result = std::from_chars(begin, end, *value);
        return result.ec == std::errc() && result.ptr == end;
    }

    inline ByteRange ParseRangeHeader(const std::string &header, uint64_t file_size)
    {
        ByteRange result;
        const std::string prefix = "bytes=";
        if (header.compare(0, prefix.size(), prefix) != 0)
            return result;

        std::string value = header.substr(prefix.size());
        if (value.empty() || value.find(',') != std::string::npos)
            return result;

        size_t dash = value.find('-');
        if (dash == std::string::npos || value.find('-', dash + 1) != std::string::npos)
            return result;

        std::string start_text = value.substr(0, dash);
        std::string end_text = value.substr(dash + 1);
        if (start_text.empty())
        {
            uint64_t suffix_length = 0;
            if (!ParseUnsigned(end_text, &suffix_length))
                return result;
            if (file_size == 0 || suffix_length == 0)
            {
                result.status = RangeStatus::UNSATISFIABLE;
                return result;
            }
            result.status = RangeStatus::PARTIAL;
            result.length = suffix_length >= file_size ? file_size : suffix_length;
            result.offset = file_size - result.length;
            return result;
        }

        uint64_t start = 0;
        if (!ParseUnsigned(start_text, &start))
            return result;
        if (file_size == 0 || start >= file_size)
        {
            result.status = RangeStatus::UNSATISFIABLE;
            return result;
        }

        uint64_t end = file_size - 1;
        if (!end_text.empty() && !ParseUnsigned(end_text, &end))
            return result;
        if (start > end)
        {
            result.status = RangeStatus::UNSATISFIABLE;
            return result;
        }
        if (end >= file_size)
            end = file_size - 1;

        result.status = RangeStatus::PARTIAL;
        result.offset = start;
        result.length = end - start + 1;
        return result;
    }
} // namespace storage
