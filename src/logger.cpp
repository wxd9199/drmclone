#include "logger.h"
#include <iostream>
#include <filesystem>

#ifdef HAVE_SPDLOG
std::shared_ptr<spdlog::logger> Logger::logger_;
#endif
LogConfig Logger::config_;

bool Logger::initialize(const LogConfig& config) {
    config_ = config;
    
#ifdef HAVE_SPDLOG
    try {
        std::vector<spdlog::sink_ptr> sinks;
        
        // 控制台输出sink
        if (config.enable_console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level(static_cast<spdlog::level::level_enum>(config.log_level));
            console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
            sinks.push_back(console_sink);
        }
        
        // 文件输出sink
        if (config.enable_file) {
            // 确保日志目录存在
            std::filesystem::path log_path(config.log_file_path);
            std::filesystem::path log_dir = log_path.parent_path();
            if (!std::filesystem::exists(log_dir)) {
                std::filesystem::create_directories(log_dir);
            }
            
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.log_file_path, 
                config.max_file_size, 
                config.max_files
            );
            file_sink->set_level(static_cast<spdlog::level::level_enum>(config.log_level));
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
            sinks.push_back(file_sink);
        }
        
        // 创建多sink logger
        logger_ = std::make_shared<spdlog::logger>("rk3588_multi_display", sinks.begin(), sinks.end());
        logger_->set_level(static_cast<spdlog::level::level_enum>(config.log_level));
        logger_->flush_on(spdlog::level::warn); // warn及以上级别立即刷新
        
        // 设置为默认logger
        spdlog::set_default_logger(logger_);
        
        // 设置flush间隔(3秒)
        spdlog::flush_every(std::chrono::seconds(3));
        
        logger_->info("Logger initialized successfully");
        logger_->info("Log file: {}", config.log_file_path);
        logger_->info("Console output: {}", config.enable_console);
        logger_->info("File output: {}", config.enable_file);
        logger_->info("Log level: {}", config.log_level);
        logger_->info("Max file size: {} MB", config.max_file_size / (1024 * 1024));
        logger_->info("Max files: {}", config.max_files);
        
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
        return false;
    }
#else
    std::cout << "[INFO] Logger initialized (fallback mode - no spdlog)" << std::endl;
    std::cout << "[INFO] Console output: " << (config.enable_console ? "enabled" : "disabled") << std::endl;
    return true;
#endif
}

void Logger::cleanup() {
#ifdef HAVE_SPDLOG
    if (logger_) {
        logger_->info("Logger shutting down");
        logger_->flush();
        spdlog::shutdown();
        logger_.reset();
    }
#endif
} 