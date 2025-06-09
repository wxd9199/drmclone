#include "system_checker.h"
#include "logger.h"
#include <cstdlib>
#include <sstream>
#include <memory>
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fstream>
#include <iostream>

SystemChecker::SystemChecker() {
}

SystemChecker::~SystemChecker() {
}

bool SystemChecker::checkStartupConditions() {
    LOG_INFO("Checking system startup conditions...");
    
    // 1. 检查是否在multi-user.target
    if (!isMultiUserTarget()) {
        LOG_WARN("System is not in multi-user.target");
        return false;
    }
    
    // 2. 检查graphical.target是否未启动或不活跃
    if (!isGraphicalTargetInactive()) {
        LOG_WARN("Graphical.target is active, not suitable for display mirroring");
        return false;
    }
    
    // 3. 检查是否有DSI显示连接
    if (!hasDSIDisplay()) {
        LOG_WARN("No DSI display found");
        return false;
    }
    
    LOG_INFO("All startup conditions satisfied");
    return true;
}

bool SystemChecker::isMultiUserTarget() {
    std::string current_target = getCurrentTarget();
    LOG_INFO("Current default target: {}", current_target);
    
    // 检查默认目标是否为multi-user.target
    bool is_multi_user = (current_target.find("multi-user.target") != std::string::npos);
    
    // 还要检查当前运行级别
    std::string active_target = executeCommand("systemctl get-default");
    if (!active_target.empty() && active_target.find("multi-user.target") != std::string::npos) {
        is_multi_user = true;
    }
    
    LOG_INFO("Multi-user target check: {}", is_multi_user ? "PASS" : "FAIL");
    return is_multi_user;
}

bool SystemChecker::isGraphicalTargetInactive() {
    bool is_inactive = !isUnitActive("graphical.target");
    LOG_INFO("Graphical target inactive check: {}", is_inactive ? "PASS" : "FAIL");
    return is_inactive;
}

bool SystemChecker::hasDSIDisplay() {
    // 检查DRM设备中是否有DSI连接器
    const char* drm_dir = "/sys/class/drm";
    DIR* dir = opendir(drm_dir);
    if (!dir) {
        LOG_ERROR("Cannot open DRM directory: {}", drm_dir);
        return false;
    }
    
    bool found_dsi = false;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        
        // 查找DSI连接器
        if (name.find("card0-DSI") != std::string::npos) {
            std::string status_path = std::string(drm_dir) + "/" + name + "/status";
            
            // 读取连接状态
            std::ifstream status_file(status_path);
            if (status_file.is_open()) {
                std::string status;
                std::getline(status_file, status);
                status_file.close();
                
                if (status == "connected") {
                    LOG_INFO("Found connected DSI display: {}", name);
                    found_dsi = true;
                    break;
                }
            }
        }
    }
    
    closedir(dir);
    
    LOG_INFO("DSI display check: {}", found_dsi ? "PASS" : "FAIL");
    return found_dsi;
}

std::string SystemChecker::executeCommand(const std::string& command) {
    std::array<char, 128> buffer;
    std::string result;
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        LOG_ERROR("Failed to execute command: {}", command);
        return "";
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    // 移除尾部换行符
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

bool SystemChecker::isUnitActive(const std::string& unit_name) {
    std::string command = "systemctl is-active " + unit_name + " 2>/dev/null";
    std::string result = executeCommand(command);
    
    bool is_active = (result == "active");
    LOG_DEBUG("Unit {} status: {}", unit_name, result);
    return is_active;
}

std::string SystemChecker::getCurrentTarget() {
    std::string result = executeCommand("systemctl get-default 2>/dev/null");
    if (result.empty()) {
        // 备用方法：检查当前运行的目标
        result = executeCommand("systemctl list-units --type=target --state=active | grep -E '(multi-user|graphical)' | head -1 | awk '{print $1}'");
    }
    return result;
} 