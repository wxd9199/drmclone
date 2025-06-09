#include "display_manager.h"
#include "frame_copier.h"
#include "logger.h"
#include "system_checker.h"
#include <iostream>
#include <signal.h>
#include <unistd.h>

static DisplayManager* g_display_manager = nullptr;

void signal_handler(int signal) {
    LOG_INFO("Received signal {}, shutting down...", signal);
    
    if (g_display_manager) {
        g_display_manager->stop();
    }
    
    Logger::cleanup();
    exit(0);
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help          Show this help message" << std::endl;
    std::cout << "  -v, --version       Show version information" << std::endl;
    std::cout << "  --verbose           Enable verbose output" << std::endl;
    std::cout << "  -d, --daemon        Run as daemon" << std::endl;
    std::cout << "  --scale-mode MODE   Scaling mode: stretch|keep-aspect (default: stretch)" << std::endl;
    std::cout << "  --rotation DEGREES  Rotation angle: 0|90|180|270 (default: 90)" << std::endl;
    std::cout << "  --quality QUALITY   Image quality: fast|good (default: good)" << std::endl;
    std::cout << "  --debug             Enable debug mode" << std::endl;
    std::cout << "Logging Options:" << std::endl;
    std::cout << "  --log-level LEVEL   Log level: 0=trace,1=debug,2=info,3=warn,4=error,5=critical (default: 2)" << std::endl;
    std::cout << "  --log-file PATH     Log file path (default: ./rk3588_multi_display.log)" << std::endl;
    std::cout << "  --no-console        Disable console output" << std::endl;
    std::cout << "  --no-file-log       Disable file logging" << std::endl;
    std::cout << std::endl;
    std::cout << "RK3588 Multi-Display Manager" << std::endl;
    std::cout << "Automatically mirrors DSI display to HDMI and DP when connected." << std::endl;
}

void print_version() {
    std::cout << "RK3588 Multi-Display Manager v1.0.0" << std::endl;
    std::cout << "Built for RK3588 platform with DRM/KMS and RGA support" << std::endl;
}

int main(int argc, char* argv[]) {
    bool run_as_daemon = false;
    bool verbose = false;
    DisplayConfig config; // 默认配置
    LogConfig log_config; // 默认日志配置
    
    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "-d" || arg == "--daemon") {
            run_as_daemon = true;
        } else if (arg == "--debug") {
            config.enable_debug = true;
        } else if (arg == "--scale-mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "stretch") {
                config.scale_mode = DisplayConfig::SCALE_STRETCH;
            } else if (mode == "keep-aspect") {
                config.scale_mode = DisplayConfig::SCALE_KEEP_ASPECT;
            } else {
                std::cerr << "Invalid scale mode: " << mode << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--rotation" && i + 1 < argc) {
            int rotation = std::stoi(argv[++i]);
            if (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) {
                config.rotation_degrees = rotation;
            } else {
                std::cerr << "Invalid rotation angle: " << rotation << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--quality" && i + 1 < argc) {
            std::string quality = argv[++i];
            if (quality == "fast") {
                config.quality = DisplayConfig::QUALITY_FAST;
            } else if (quality == "good") {
                config.quality = DisplayConfig::QUALITY_GOOD;
            } else {
                std::cerr << "Invalid quality setting: " << quality << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--log-level" && i + 1 < argc) {
            int level = std::stoi(argv[++i]);
            if (level >= 0 && level <= 5) {
                log_config.log_level = level;
            } else {
                std::cerr << "Invalid log level: " << level << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else if (arg == "--log-file" && i + 1 < argc) {
            log_config.log_file_path = argv[++i];
        } else if (arg == "--no-console") {
            log_config.enable_console = false;
        } else if (arg == "--no-file-log") {
            log_config.enable_file = false;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // 初始化日志系统
    if (!Logger::initialize(log_config)) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }
    
    print_version();
    LOG_INFO("Starting RK3588 Multi-Display Manager...");
    
    if (verbose) {
        LOG_INFO("Verbose mode enabled");
    }
    
    // 系统条件检查
    SystemChecker system_checker;
    if (!system_checker.checkStartupConditions()) {
        LOG_ERROR("System startup conditions not met, exiting");
        Logger::cleanup();
        return 1;
    }
    
    // 设置信号处理器
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 创建显示管理器
    DisplayManager display_manager;
    g_display_manager = &display_manager;
    
    // 初始化显示管理器
    if (!display_manager.initialize()) {
        LOG_ERROR("Failed to initialize display manager");
        Logger::cleanup();
        return 1;
    }
    
    // 应用配置
    display_manager.setDisplayConfig(config);
    
    // 如果以守护进程模式运行
    if (run_as_daemon) {
        LOG_INFO("Running as daemon...");
        
        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERROR("Failed to fork daemon process");
            Logger::cleanup();
            return 1;
        }
        
        if (pid > 0) {
            // 父进程退出
            LOG_INFO("Daemon started with PID: {}", pid);
            Logger::cleanup();
            return 0;
        }
        
        // 子进程继续运行
        setsid(); // 创建新会话
        
        // 重定向标准输入输出到/dev/null (但保留日志文件)
        freopen("/dev/null", "r", stdin);
        if (!log_config.enable_console) {
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
        }
    }
    
    // 启动显示管理器
    display_manager.run();
    
    LOG_INFO("Display manager is running. Press Ctrl+C to stop.");
    
    // 主循环 - 等待信号
    while (true) {
        sleep(1);
    }
    
    // 清理资源
    LOG_INFO("Shutting down display manager...");
    display_manager.cleanup();
    g_display_manager = nullptr;
    
    // 清理日志系统
    Logger::cleanup();
    
    return 0;
} 