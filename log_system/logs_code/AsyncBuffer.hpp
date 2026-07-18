#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

#include "Util.hpp"

extern mylog::Util::JsonData *g_conf_data;

namespace mylog
{
    class Buffer
    {
    public:
        Buffer()
        {
            if (g_conf_data == nullptr || g_conf_data->buffer_size == 0)
                throw std::runtime_error("log configuration is not initialized");
            buffer_.resize(g_conf_data->buffer_size);
        }

        void Push(const char *data, size_t len)
        {
            if (len == 0)
                return;
            if (data == nullptr)
                throw std::invalid_argument("cannot append null log data");
            EnsureWritable(len);
            std::copy_n(data, len, buffer_.data() + write_pos_);
            write_pos_ += len;
        }

        bool IsEmpty() const { return write_pos_ == read_pos_; }
        size_t Capacity() const { return buffer_.size(); }
        size_t WriteableSize() const { return buffer_.size() - write_pos_; }
        size_t ReadableSize() const { return write_pos_ - read_pos_; }
        const char *Begin() const { return buffer_.data() + read_pos_; }

        void Swap(Buffer &other)
        {
            buffer_.swap(other.buffer_);
            std::swap(read_pos_, other.read_pos_);
            std::swap(write_pos_, other.write_pos_);
        }

        void Reset()
        {
            write_pos_ = 0;
            read_pos_ = 0;
        }

    private:
        void EnsureWritable(size_t len)
        {
            if (len <= WriteableSize())
                return;
            if (write_pos_ > std::numeric_limits<size_t>::max() - len)
                throw std::length_error("log buffer size overflow");

            const size_t required = write_pos_ + len;
            size_t new_size = buffer_.size();
            while (new_size < required)
            {
                size_t growth = new_size < g_conf_data->threshold
                                    ? std::max(new_size, static_cast<size_t>(1))
                                    : g_conf_data->linear_growth;
                if (growth > std::numeric_limits<size_t>::max() - new_size)
                {
                    new_size = required;
                    break;
                }
                new_size += growth;
            }
            buffer_.resize(new_size);
        }

    private:
        std::vector<char> buffer_;
        size_t write_pos_ = 0;
        size_t read_pos_ = 0;
    };
} // namespace mylog
