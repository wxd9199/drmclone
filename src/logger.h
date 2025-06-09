#pragma once

#ifdef HAVE_SPDLOG
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#endif

#include <string>
#include <memory>

struct LogConfig {
    std::string log_file_path = "./rk3588_multi_display.log"; // 默认当前文件夹
    bool enable_console = true;
    bool enable_file = false;
    int log_level = 2; // 0=trace, 1=debug, 2=info, 3=warn, 4=error, 5=critical
    size_t max_file_size = 20 * 1024 * 1024; // 20MB
    size_t max_files = 7; // 7 days rotation
};

class Logger {
public:
    static bool initialize(const LogConfig& config);
    static void cleanup();
    
    // 日志宏定义
    template<typename... Args>
    static void trace(const char* fmt, Args&&... args) {
#ifdef HAVE_SPDLOG
        if (logger_) logger_->trace(fmt, std::forward<Args>(args)...);
#endif
    }
    
    template<typename... Args>
    static void debug(const char* fmt, Args&&... args) {
#ifdef HAVE_SPDLOG
        if (logger_) logger_->debug(fmt, std::forward<Args>(args)...);
#else
        printf("[DEBUG] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
#endif
    }
    
    template<typename... Args>
    static void info(const char* fmt, Args&&... args) {
#ifdef HAVE_SPDLOG
        if (logger_) logger_->info(fmt, std::forward<Args>(args)...);
#else
        printf("[INFO] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
#endif
    }
    
    template<typename... Args>
    static void warn(const char* fmt, Args&&... args) {
#ifdef HAVE_SPDLOG
        if (logger_) logger_->warn(fmt, std::forward<Args>(args)...);
#else
        printf("[WARN] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
#endif
    }
    
    template<typename... Args>
    static void error(const char* fmt, Args&&... args) {
#ifdef HAVE_SPDLOG
        if (logger_) logger_->error(fmt, std::forward<Args>(args)...);
#else
        printf("[ERROR] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
#endif
    }
    
    template<typename... Args>
    static void critical(const char* fmt, Args&&... args) {
#ifdef HAVE_SPDLOG
        if (logger_) logger_->critical(fmt, std::forward<Args>(args)...);
#else
        printf("[CRITICAL] ");
        printf(fmt, std::forward<Args>(args)...);
        printf("\n");
#endif
    }

private:
#ifdef HAVE_SPDLOG
    static std::shared_ptr<spdlog::logger> logger_;
#endif
    static LogConfig config_;
};

// 便捷宏
#define LOG_TRACE(...) Logger::trace(__VA_ARGS__)
#define LOG_DEBUG(...) Logger::debug(__VA_ARGS__)
#define LOG_INFO(...) Logger::info(__VA_ARGS__)
#define LOG_WARN(...) Logger::warn(__VA_ARGS__)
#define LOG_ERROR(...) Logger::error(__VA_ARGS__)
#define LOG_CRITICAL(...) Logger::critical(__VA_ARGS__) 