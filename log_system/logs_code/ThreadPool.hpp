#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool
{
public:
    explicit ThreadPool(size_t thread_count, size_t max_pending_tasks = 1024)
        : max_pending_tasks_(max_pending_tasks)
    {
        if (thread_count == 0)
            throw std::invalid_argument("ThreadPool requires at least one worker");
        if (max_pending_tasks_ == 0)
            throw std::invalid_argument("ThreadPool queue size must be greater than zero");

        workers_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i)
        {
            workers_.emplace_back([this]() { WorkerLoop(); });
        }
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    template <class F, class... Args>
    auto Enqueue(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using ReturnType = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<ReturnType> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            space_available_.wait(lock, [this]() {
                return stopping_ || tasks_.size() < max_pending_tasks_;
            });
            if (stopping_)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks_.emplace([task]() { (*task)(); });
        }
        task_available_.notify_one();
        return result;
    }

    template <class F, class... Args>
    bool TryEnqueue(F &&f, Args &&...args)
    {
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || tasks_.size() >= max_pending_tasks_)
                return false;
            tasks_.emplace(std::move(task));
        }
        task_available_.notify_one();
        return true;
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        task_available_.notify_all();
        space_available_.notify_all();
        for (std::thread &worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
    }

private:
    void WorkerLoop()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                task_available_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty())
                    return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            space_available_.notify_one();
            task();
        }
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable task_available_;
    std::condition_variable space_available_;
    size_t max_pending_tasks_;
    bool stopping_ = false;
};
