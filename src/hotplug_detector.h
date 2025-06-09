#pragma once

#include <functional>
#include <thread>
#include <atomic>
#include <string>
#include <libudev.h>

enum class HotplugEvent {
    CONNECTED,
    DISCONNECTED
};

using HotplugCallback = std::function<void(const std::string& connector_name, HotplugEvent event)>;

class HotplugDetector {
public:
    HotplugDetector();
    ~HotplugDetector();
    
    bool initialize();
    void cleanup();
    
    void setCallback(HotplugCallback callback);
    void start();
    void stop();
    
private:
    std::atomic<bool> running_;
    std::thread monitor_thread_;
    HotplugCallback callback_;
    
    struct udev* udev_;
    struct udev_monitor* monitor_;
    int monitor_fd_;
    
    void monitorLoop();
    void processUdevDevice(struct udev_device* device);
    void checkAllConnectors();
    std::string getConnectorNameFromSysPath(const char* syspath);
    HotplugEvent getConnectorStatus(const char* syspath);
}; 