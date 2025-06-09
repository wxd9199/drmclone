#include "display_manager.h"
#include "logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <set>

DisplayManager::DisplayManager()
    : running_(false), copy_enabled_(false), primary_display_(nullptr) {
}

DisplayManager::~DisplayManager() {
    cleanup();
}

bool DisplayManager::initialize() {
    // 创建DRM管理器
    drm_manager_ = std::make_shared<DRMManager>();
    if (!drm_manager_->initialize()) {
        LOG_ERROR("Failed to initialize DRM manager");
        return false;
    }
    
    // 创建RGA助手
    rga_helper_ = std::make_shared<RGAHelper>();
    if (!rga_helper_->initialize()) {
        LOG_ERROR("Failed to initialize RGA helper");
        return false;
    }
    
    // 创建帧复制器
    frame_copier_ = std::make_shared<FrameCopier>(drm_manager_, rga_helper_);
    if (!frame_copier_->initialize()) {
        LOG_ERROR("Failed to initialize frame copier");
        return false;
    }
    
    // 创建热插拔检测器
    hotplug_detector_ = std::make_shared<HotplugDetector>();
    if (!hotplug_detector_->initialize()) {
        LOG_ERROR("Failed to initialize hotplug detector");
        return false;
    }
    
    // 设置热插拔回调
    hotplug_detector_->setCallback(
        [this](const std::string& connector_name, HotplugEvent event) {
            onHotplugEvent(connector_name, event);
        }
    );
    
    // 初始化显示器状态
    updateDisplays();
    
    // 初始化复制状态
    updateCopyState();
    
    LOG_INFO("Display manager initialized successfully");
    return true;
}

void DisplayManager::cleanup() {
    stop();
    
    if (hotplug_detector_) {
        hotplug_detector_->cleanup();
        hotplug_detector_.reset();
    }
    
    if (frame_copier_) {
        frame_copier_->cleanup();
        frame_copier_.reset();
    }
    
    if (rga_helper_) {
        rga_helper_->cleanup();
        rga_helper_.reset();
    }
    
    if (drm_manager_) {
        drm_manager_->cleanup();
        drm_manager_.reset();
    }
}

void DisplayManager::run() {
    if (running_) {
        return;
    }
    
    running_ = true;
    
    // 启动热插拔监控
    hotplug_detector_->start();
    
    // 启动帧复制线程
    copy_thread_ = std::thread(&DisplayManager::copyLoop, this);
    
    LOG_INFO("Display manager started");
}

void DisplayManager::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // 停止热插拔监控
    if (hotplug_detector_) {
        hotplug_detector_->stop();
    }
    
    // 等待复制线程结束
    if (copy_thread_.joinable()) {
        copy_thread_.join();
    }
    
    LOG_INFO("Display manager stopped");
}

void DisplayManager::setDisplayConfig(const DisplayConfig& config) {
    if (frame_copier_) {
        frame_copier_->setConfig(config);
        LOG_INFO("Display configuration updated: scale={}, rotation={}°, quality={}, debug={}", 
                (config.scale_mode == DisplayConfig::SCALE_STRETCH ? "stretch" : "keep-aspect"),
                config.rotation_degrees,
                (config.quality == DisplayConfig::QUALITY_FAST ? "fast" : "good"),
                (config.enable_debug ? "enabled" : "disabled"));
    }
}

void DisplayManager::onHotplugEvent(const std::string& connector_name, HotplugEvent event) {
    std::lock_guard<std::mutex> lock(display_mutex_);
    
    LOG_INFO("Processing hotplug event: {} {}", connector_name, 
             (event == HotplugEvent::CONNECTED ? "connected" : "disconnected"));
    
    // 重新扫描显示器
    drm_manager_->scanDisplays();
    updateDisplays();
    
    // 更新复制状态
    updateCopyState();
}

void DisplayManager::updateDisplays() {
    auto displays = drm_manager_->getDisplays();
    
    // 查找主显示器
    primary_display_ = drm_manager_->getPrimaryDisplay();
    if (!primary_display_) {
        LOG_WARN("No primary display found");
    }
    
    // 智能更新副显示器列表，只处理状态变化的显示器
    std::set<uint32_t> old_secondary_ids(secondary_display_ids_.begin(), secondary_display_ids_.end());
    std::set<uint32_t> new_secondary_ids;
    
    for (auto& display : displays) {
        if (!display.is_primary && isSecondaryDisplay(display.name)) {
            new_secondary_ids.insert(display.connector_id);
            
            // 检查是否是新连接或重新连接的显示器
            bool was_enabled = (old_secondary_ids.count(display.connector_id) > 0);
            bool should_enable = display.connected;
            
            if (should_enable && !was_enabled) {
                // 新连接的显示器，启用它
                LOG_INFO("New display connected: {}", display.name);
                enableSecondaryDisplay(&display);
            } else if (!should_enable && was_enabled) {
                // 断开连接的显示器，禁用它
                LOG_INFO("Display disconnected: {}", display.name);
                disableSecondaryDisplay(&display);
            } else if (should_enable && was_enabled) {
                // 显示器重新连接，可能需要重新创建缓冲区
                LOG_INFO("Display reconnected, refreshing buffers: {}", display.name);
                disableSecondaryDisplay(&display);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                enableSecondaryDisplay(&display);
            }
            // 对于已经正确配置的显示器，不做任何操作避免黑屏
        }
    }
    
    secondary_display_ids_.assign(new_secondary_ids.begin(), new_secondary_ids.end());
    
    LOG_INFO("Updated displays: {} secondary displays found", secondary_display_ids_.size());
}

void DisplayManager::enableSecondaryDisplay(DisplayInfo* display) {
    if (!display || !display->connected) {
        return;
    }

    LOG_INFO("Enabling secondary display: {}", display->name);
    LOG_DEBUG("Display details: connector_id={}, encoder_id={}, crtc_id={}, mode={}x{}@{}Hz", 
              display->connector_id, display->encoder_id, display->crtc_id,
              display->mode.hdisplay, display->mode.vdisplay, display->mode.vrefresh);
    
    // 首先禁用显示器以清理之前的状态
    drm_manager_->disableDisplay(display);
    
    // 等待硬件准备就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 创建显示器缓冲区
    if (!frame_copier_->createBuffersForDisplay(display)) {
        LOG_ERROR("Failed to create buffers for {}", display->name);
        return;
    }

    // 获取当前缓冲区的framebuffer ID
    GBMBuffer* buffer = frame_copier_->getCurrentBuffer(display);
    if (!buffer || !buffer->fb_id) {
        LOG_ERROR("Failed to get framebuffer for {}", display->name);
        frame_copier_->destroyBuffersForDisplay(display);
        return;
    }

    // 启用显示器，带重试机制
    bool enabled = false;
    for (int retry = 0; retry < 3; retry++) {
        if (drm_manager_->setCRTCWithFramebuffer(display, buffer->fb_id)) {
            enabled = true;
            break;
        }
        LOG_WARN("Failed to enable display {} (attempt {}/3), retrying...", display->name, retry + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    if (!enabled) {
        LOG_ERROR("Failed to enable display {} after 3 attempts", display->name);
        frame_copier_->destroyBuffersForDisplay(display);
        return;
    }

    LOG_INFO("Successfully enabled display {}", display->name);
}

void DisplayManager::disableSecondaryDisplay(DisplayInfo* display) {
    if (!display) {
        return;
    }
    
    LOG_INFO("Disabling secondary display: {}", display->name);
    
    // 禁用显示器
    drm_manager_->disableDisplay(display);
    
    // 销毁显示器缓冲区
    frame_copier_->destroyBuffersForDisplay(display);
    
    LOG_INFO("Successfully disabled display {}", display->name);
}

void DisplayManager::copyLoop() {
    LOG_INFO("Frame copy loop started");
    
    const auto target_frame_time = std::chrono::microseconds(16667); // 60 FPS = 16.667ms per frame (提高目标帧率)
    auto last_frame_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    auto fps_start_time = last_frame_time;
    
    while (running_) {
        auto frame_start = std::chrono::steady_clock::now();
        
        // 只有当有活跃的副显示器时才进行复制
        if (copy_enabled_.load()) {
            copyFrameToSecondaryDisplays();
            
            // 非阻塞的等待页面翻转事件
            drm_manager_->waitForPageFlipEvents(1); // 只等待1ms，不阻塞
            
            frame_count++;
        } else {
            // 没有副显示器时，降低CPU占用
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // 每30秒报告一次FPS (减少日志频率)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - fps_start_time);
        if (elapsed.count() >= 300 && copy_enabled_.load()) {
            double fps = (double)frame_count / elapsed.count();
            LOG_INFO("Frame rate: {:.1f} FPS (avg over {}s)", fps, elapsed.count());
            frame_count = 0;
            fps_start_time = now;
        }
        
        // 精确的帧率控制（仅在复制启用时）
        if (copy_enabled_.load()) {
            auto frame_end = std::chrono::steady_clock::now();
            auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_start);
            
            if (frame_duration < target_frame_time) {
                auto sleep_time = target_frame_time - frame_duration;
                std::this_thread::sleep_for(sleep_time);
            }
        }
        
        last_frame_time = frame_start;
    }
    
    LOG_INFO("Frame copy loop stopped");
}

void DisplayManager::copyFrameToSecondaryDisplays() {
    std::lock_guard<std::mutex> lock(display_mutex_);
    
    if (!primary_display_ || !primary_display_->connected) {
        return;
    }
    
    // 检查是否有连接的副显示器
    bool has_active_secondary = false;
    auto displays = drm_manager_->getDisplays();
    for (uint32_t connector_id : secondary_display_ids_) {
        for (auto& display : displays) {
            if (display.connector_id == connector_id && display.connected) {
                has_active_secondary = true;
                break;
            }
        }
        if (has_active_secondary) break;
    }
    
    if (!has_active_secondary) {
        return;
    }
    
    // 从主显示器捕获帧
    FrameBuffer source_frame;
    if (!frame_copier_->captureFrame(primary_display_, source_frame)) {
        return;
    }
    
    // 复制到所有连接的副显示器
    for (uint32_t connector_id : secondary_display_ids_) {
        for (auto& display : displays) {
            if (display.connector_id == connector_id && display.connected) {
                frame_copier_->copyToDisplay(source_frame, &display);
                break;
            }
        }
    }
    
    // 释放源帧缓冲区
    rga_helper_->freeBuffer(source_frame);
}

bool DisplayManager::isSecondaryDisplay(const std::string& name) {
    // 检查是否是HDMI或DP显示器 (匹配DRM生成的名称格式)
    return (name.find("HDMI") != std::string::npos || 
            name.find("DisplayPort") != std::string::npos);
}

DisplayInfo* DisplayManager::findDisplayByName(const std::string& name) {
    auto displays = drm_manager_->getDisplays();
    for (auto& display : displays) {
        if (display.name == name) {
            return &display;
        }
    }
    return nullptr;
}

bool DisplayManager::hasActiveSecondaryDisplays() {
    auto displays = drm_manager_->getDisplays();
    for (uint32_t connector_id : secondary_display_ids_) {
        for (auto& display : displays) {
            if (display.connector_id == connector_id && display.connected) {
                return true;
            }
        }
    }
    return false;
}

void DisplayManager::updateCopyState() {
    bool should_copy = hasActiveSecondaryDisplays();
    bool was_copying = copy_enabled_.load();
    
    copy_enabled_.store(should_copy);
    
    if (should_copy && !was_copying) {
        LOG_INFO("Frame copying enabled - secondary displays connected");
    } else if (!should_copy && was_copying) {
        LOG_INFO("Frame copying disabled - no secondary displays connected");
    }
} 