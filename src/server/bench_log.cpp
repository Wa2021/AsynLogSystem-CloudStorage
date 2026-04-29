#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include "../../log_system/logs_code/MyLog.hpp"
#include "../../log_system/logs_code/Util.hpp"

using namespace std::chrono;

mylog::Util::JsonData* g_conf_data;
ThreadPool* tp = nullptr;

void init_log() {
    g_conf_data = mylog::Util::JsonData::GetJsonData();
    tp = new ThreadPool(g_conf_data->thread_count);
    std::shared_ptr<mylog::LoggerBuilder> Glb(new mylog::LoggerBuilder());
    Glb->BuildLoggerName("benchlogger");
    // 这里使用纯写文件，屏蔽向终端打印，以测磁盘IO极限
    Glb->BuildLoggerFlush<mylog::FileFlush>("./logfile/bench.log");
    mylog::LoggerManager::GetInstance().AddLogger(Glb->Build());
}

int main() {
    init_log();
    int thread_count = 4;        // 模拟 4 个工作线程并发写
    int logs_per_thread = 500000; // 每个线程写 50W 条该日志
    
    // 构造一个固定长度的大概 100 Byte 的测试字符串
    std::string test_msg(80, 'A'); 
    
    auto start = high_resolution_clock::now();
    
    std::vector<std::thread> threads;
    auto logger = mylog::GetLogger("benchlogger");
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < logs_per_thread; ++j) {
                logger->Info("Bench test message: %s", test_msg.c_str());
            }
        });
    }
    
    for (auto& t : threads) { t.join(); }
    
    // 等到异步缓存的剩余日志全刷完(可在这个位置睡眠2秒让异步队列清理空)
    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    // 计算
    long long total_logs = static_cast<long long>(thread_count) * logs_per_thread;
    long long total_bytes = total_logs * 100; // 粗略估算一条100字节
    double total_mb = static_cast<double>(total_bytes) / (1024 * 1024);
    double speed = total_mb / (duration.count() / 1000.0);
    
    std::cout << "测试耗时: " << duration.count() << " ms" << std::endl;
    std::cout << "总输出量: " << total_mb << " MB" << std::endl;
    std::cout << "吞吐量: " << speed << " MB/s" << std::endl;
    
    return 0;
}
