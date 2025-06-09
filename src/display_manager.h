#pragma once

#include "drm_manager.h"
#include "hotplug_detector.h"
#include "frame_copier.h"
#include "rga_helper.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();
    
    bool initialize();
    void cleanup();
    
    void run();
    void stop();
    
    // 配置管理
    void setDisplayConfig(const DisplayConfig& config);
    
private:
    std::shared_ptr<DRMManager> drm_manager_;
    std::shared_ptr<FrameCopier> frame_copier_;
    std::shared_ptr<RGAHelper> rga_helper_;
    std::shared_ptr<HotplugDetector> hotplug_detector_;
    
    DisplayInfo* primary_display_;
    std::vector<uint32_t> secondary_display_ids_;  // Store connector IDs instead of pointers
    
    std::atomic<bool> running_;
    std::atomic<bool> copy_enabled_;  // 控制是否需要复制帧
    std::thread copy_thread_;
    std::mutex display_mutex_;
    
    // 回调函数
    void onHotplugEvent(const std::string& connector_name, HotplugEvent event);
    
    // 显示器管理
    void updateDisplays();
    void enableSecondaryDisplay(DisplayInfo* display);
    void disableSecondaryDisplay(DisplayInfo* display);
    
    // 主循环
    void copyLoop();
    
    // 帧复制逻辑
    void copyFrameToSecondaryDisplays();
    
    // 工具函数
    bool isSecondaryDisplay(const std::string& name);
    DisplayInfo* findDisplayByName(const std::string& name);
    
    // 连接状态管理
    bool hasActiveSecondaryDisplays();
    void updateCopyState();
}; 