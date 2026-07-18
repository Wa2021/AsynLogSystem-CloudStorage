#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "AsyncBuffer.hpp"

namespace mylog
{
    enum class AsyncType
    {
        ASYNC_SAFE,
        ASYNC_UNSAFE
    };

    using FlushCallback = std::function<void(Buffer &)>;

    class AsyncWorker
    {
    public:
        using ptr = std::shared_ptr<AsyncWorker>;

        explicit AsyncWorker(FlushCallback callback,
                             AsyncType async_type = AsyncType::ASYNC_SAFE)
            : async_type_(async_type), callback_(std::move(callback)),
              thread_(&AsyncWorker::ThreadEntry, this)
        {
        }

        AsyncWorker(const AsyncWorker &) = delete;
        AsyncWorker &operator=(const AsyncWorker &) = delete;

        ~AsyncWorker() { Stop(); }

        bool Push(const char *data, size_t len)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (async_type_ == AsyncType::ASYNC_SAFE)
            {
                producer_ready_.wait(lock, [this, len]() {
                    return stopping_ || buffer_producer_.IsEmpty() ||
                           len <= buffer_producer_.WriteableSize();
                });
            }
            if (stopping_)
                return false;

            buffer_producer_.Push(data, len);
            consumer_ready_.notify_one();
            return true;
        }

        void Stop()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_)
                    return;
                stopping_ = true;
            }
            consumer_ready_.notify_all();
            producer_ready_.notify_all();
            if (thread_.joinable())
                thread_.join();
        }

    private:
        void ThreadEntry()
        {
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    consumer_ready_.wait(lock, [this]() {
                        return stopping_ || !buffer_producer_.IsEmpty();
                    });
                    if (stopping_ && buffer_producer_.IsEmpty())
                        return;
                    buffer_producer_.Swap(buffer_consumer_);
                    producer_ready_.notify_all();
                }

                if (!buffer_consumer_.IsEmpty())
                    callback_(buffer_consumer_);
                buffer_consumer_.Reset();
            }
        }

    private:
        AsyncType async_type_;
        FlushCallback callback_;
        std::mutex mutex_;
        Buffer buffer_producer_;
        Buffer buffer_consumer_;
        std::condition_variable producer_ready_;
        std::condition_variable consumer_ready_;
        bool stopping_ = false;
        std::thread thread_;
    };
} // namespace mylog
