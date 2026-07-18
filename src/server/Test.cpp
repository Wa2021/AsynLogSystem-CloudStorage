#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>

#include "Service.hpp"
#include "../../log_system/logs_code/MyLog.hpp"
#include "../../log_system/logs_code/ThreadPool.hpp"

ThreadPool *tp = nullptr;
mylog::Util::JsonData *g_conf_data = nullptr;

int main(int argc, char *argv[])
{
    if (argc > 2)
    {
        std::cerr << "usage: " << argv[0] << " [Storage.conf]" << std::endl;
        return 2;
    }
    if (argc == 2)
        setenv("CLOUD_STORAGE_CONFIG", argv[1], 1);

    std::unique_ptr<ThreadPool> thread_pool;
    bool logger_initialized = false;
    try
    {
        g_conf_data = mylog::Util::JsonData::GetJsonData();
        thread_pool = std::make_unique<ThreadPool>(
            g_conf_data->thread_count, g_conf_data->backup_queue_size);
        tp = thread_pool.get();

        mylog::LoggerBuilder builder;
        builder.BuildLoggerName("asynclogger");
        builder.BuildLoggerFlush<mylog::RollFileFlush>("./logfile/cloud-storage",
                                                       1024 * 1024);
        if (!mylog::LoggerManager::GetInstance().AddLogger(builder.Build()))
            throw std::runtime_error("cannot register application logger");
        logger_initialized = true;

        storage::DataManager data_manager;
        storage::Service service(data_manager);
        bool success = service.RunModule();

        mylog::LoggerManager::GetInstance().Shutdown();
        logger_initialized = false;
        thread_pool.reset();
        tp = nullptr;
        return success ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        std::cerr << "startup failed: " << error.what() << std::endl;
        if (logger_initialized)
            mylog::LoggerManager::GetInstance().Shutdown();
        thread_pool.reset();
        tp = nullptr;
        return 1;
    }
}
