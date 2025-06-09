#include "hotplug_detector.h"
#include "logger.h"
#include <poll.h>
#include <iostream>
#include <cstring>
#include <fstream>
#include <map>

HotplugDetector::HotplugDetector()
    : running_(false), udev_(nullptr), monitor_(nullptr), monitor_fd_(-1) {
}

HotplugDetector::~HotplugDetector() {
    cleanup();
}

bool HotplugDetector::initialize() {
    // 初始化udev
    udev_ = udev_new();
    if (!udev_) {
        LOG_ERROR("Failed to create udev context");
        return false;
    }
    
    // 创建监视器
    monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
    if (!monitor_) {
        LOG_ERROR("Failed to create udev monitor");
        udev_unref(udev_);
        udev_ = nullptr;
        return false;
    }
    
    // 设置过滤器，只监听DRM设备
    int ret = udev_monitor_filter_add_match_subsystem_devtype(monitor_, "drm", nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to add udev filter");
        cleanup();
        return false;
    }
    
    // 启用监视器
    ret = udev_monitor_enable_receiving(monitor_);
    if (ret < 0) {
        LOG_ERROR("Failed to enable udev monitor");
        cleanup();
        return false;
    }
    
    // 获取监视器文件描述符
    monitor_fd_ = udev_monitor_get_fd(monitor_);
    if (monitor_fd_ < 0) {
        LOG_ERROR("Failed to get udev monitor fd");
        cleanup();
        return false;
    }
    
    LOG_INFO("Hotplug detector initialized successfully");
    return true;
}

void HotplugDetector::cleanup() {
    stop();
    
    if (monitor_) {
        udev_monitor_unref(monitor_);
        monitor_ = nullptr;
    }
    
    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }
    
    monitor_fd_ = -1;
}

void HotplugDetector::setCallback(HotplugCallback callback) {
    callback_ = callback;
}

void HotplugDetector::start() {
    if (running_ || monitor_fd_ < 0) {
        return;
    }
    
    running_ = true;
    monitor_thread_ = std::thread(&HotplugDetector::monitorLoop, this);
    
    LOG_INFO("Hotplug monitoring started");
}

void HotplugDetector::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    LOG_INFO("Hotplug monitoring stopped");
}

void HotplugDetector::monitorLoop() {
    struct pollfd pfd;
    pfd.fd = monitor_fd_;
    pfd.events = POLLIN;
    
    while (running_) {
        // 等待事件，超时时间1秒
        int ret = poll(&pfd, 1, 1000);
        
        if (ret < 0) {
            if (errno != EINTR) {
                LOG_ERROR("Poll error: {}", strerror(errno));
                break;
            }
            continue;
        }
        
        if (ret == 0) {
            // 超时，继续循环
            continue;
        }
        
        if (pfd.revents & POLLIN) {
            // 有事件到达
            struct udev_device* device = udev_monitor_receive_device(monitor_);
            if (device) {
                processUdevDevice(device);
                udev_device_unref(device);
            }
        }
    }
}

void HotplugDetector::processUdevDevice(struct udev_device* device) {
    const char* action = udev_device_get_action(device);
    const char* subsystem = udev_device_get_subsystem(device);
    const char* syspath = udev_device_get_syspath(device);
    
    if (!action || !subsystem || !syspath) {
        return;
    }
    
    // 检查是否是DRM子系统的change事件
    if (strcmp(subsystem, "drm") != 0 || strcmp(action, "change") != 0) {
        return;
    }
    
    // 检查是否是card0的事件
    std::string path = syspath;
    if (path.find("/drm/card0") == std::string::npos) {
        return;
    }
    
    LOG_DEBUG("DRM change event detected for card0, checking all connectors...");
    
    // 当检测到card0变化时，检查所有连接器状态
    checkAllConnectors();
}

void HotplugDetector::checkAllConnectors() {
    // 存储当前状态用于比较
    static std::map<std::string, bool> previous_states;
    
    // 检查HDMI连接器
    std::string hdmi_path = "/sys/class/drm/card0-HDMI-A-1";
    std::string hdmi_status_file = hdmi_path + "/status";
    
    std::ifstream hdmi_file(hdmi_status_file);
    if (hdmi_file.is_open()) {
        std::string status;
        std::getline(hdmi_file, status);
        hdmi_file.close();
        
        bool connected = (status == "connected");
        bool prev_connected = previous_states["HDMI"];
        
        if (connected != prev_connected) {
            LOG_INFO("HDMI hotplug detected: card0-HDMI-A-1 -> {}", status);
            previous_states["HDMI"] = connected;
            
            if (callback_) {
                callback_("card0-HDMI-A-1", connected ? HotplugEvent::CONNECTED : HotplugEvent::DISCONNECTED);
            }
        }
    }
    
    // 检查DisplayPort连接器
    std::string dp_path = "/sys/class/drm/card0-DP-1";
    std::string dp_status_file = dp_path + "/status";
    
    std::ifstream dp_file(dp_status_file);
    if (dp_file.is_open()) {
        std::string status;
        std::getline(dp_file, status);
        dp_file.close();
        
        bool connected = (status == "connected");
        bool prev_connected = previous_states["DP"];
        
        if (connected != prev_connected) {
            LOG_INFO("DisplayPort hotplug detected: card0-DP-1 -> {}", status);
            previous_states["DP"] = connected;
            
            if (callback_) {
                callback_("card0-DP-1", connected ? HotplugEvent::CONNECTED : HotplugEvent::DISCONNECTED);
            }
        }
    }
}

std::string HotplugDetector::getConnectorNameFromSysPath(const char* syspath) {
    if (!syspath) {
        return "";
    }
    
    std::string path = syspath;
    
    // 查找连接器名称，格式通常为 /sys/devices/.../drm/card0/card0-HDMI-A-1
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos) {
        std::string connector_name = path.substr(pos + 1);
        
        // 检查是否是连接器名称格式 (card0-XXX-X)
        if (connector_name.find("card0-") == 0) {
            return connector_name;
        }
    }
    
    return "";
}

HotplugEvent HotplugDetector::getConnectorStatus(const char* syspath) {
    if (!syspath) {
        return HotplugEvent::DISCONNECTED;
    }
    
    std::string status_file = std::string(syspath) + "/status";
    std::ifstream file(status_file);
    
    if (!file.is_open()) {
        LOG_DEBUG("Cannot open status file: {}", status_file);
        return HotplugEvent::DISCONNECTED;
    }
    
    std::string status;
    std::getline(file, status);
    file.close();
    
    LOG_DEBUG("Connector status from {}: {}", status_file, status);
    
    if (status == "connected") {
        return HotplugEvent::CONNECTED;
    } else if (status == "disconnected") {
        return HotplugEvent::DISCONNECTED;
    }
    
    return HotplugEvent::DISCONNECTED;
} 