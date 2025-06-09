#include "drm_manager.h"
#include "logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <algorithm>

DRMManager::DRMManager() 
    : drm_fd_(-1), resources_(nullptr) {
}

DRMManager::~DRMManager() {
    cleanup();
}

bool DRMManager::initialize(const char* device_path) {
    // 打开DRM设备
    drm_fd_ = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd_ < 0) {
        LOG_ERROR("Failed to open DRM device: {}", device_path);
        return false;
    }

    // 检查DRM设备能力
    if (!probeDrmDevice()) {
        close(drm_fd_);
        drm_fd_ = -1;
        return false;
    }

    // 获取DRM资源
    resources_ = drmModeGetResources(drm_fd_);
    if (!resources_) {
        LOG_ERROR("Failed to get DRM resources");
        close(drm_fd_);
        drm_fd_ = -1;
        return false;
    }

    // 扫描显示器
    if (!scanDisplays()) {
        LOG_ERROR("Failed to scan displays");
        cleanup();
        return false;
    }

    return true;
}

void DRMManager::cleanup() {
    displays_.clear();
    
    if (resources_) {
        drmModeFreeResources(resources_);
        resources_ = nullptr;
    }
    
    if (drm_fd_ >= 0) {
        close(drm_fd_);
        drm_fd_ = -1;
    }
}

bool DRMManager::probeDrmDevice() {
    // 检查设备是否支持KMS
    uint64_t has_dumb;
    if (drmGetCap(drm_fd_, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
        LOG_ERROR("DRM device doesn't support dumb buffers");
        return false;
    }
    
    return true;
}

bool DRMManager::scanDisplays() {
    displays_.clear();
    
    for (int i = 0; i < resources_->count_connectors; i++) {
        uint32_t connector_id = resources_->connectors[i];
        DisplayInfo info;
        
        if (getConnectorInfo(connector_id, info)) {
            displays_.push_back(info);
        }
    }
    
    // 设置主显示器 (DSI-1)
    for (auto& display : displays_) {
        if (display.name.find("DSI-1") != std::string::npos) {
            display.is_primary = true;
            break;
        }
    }
    
    LOG_INFO("Found {} displays:", displays_.size());
    for (const auto& display : displays_) {
        LOG_INFO("  {} ({}x{}) - {}{}", 
                display.name, display.width, display.height,
                (display.connected ? "connected" : "disconnected"),
                (display.is_primary ? " [PRIMARY]" : ""));
    }
    
    return !displays_.empty();
}

bool DRMManager::getConnectorInfo(uint32_t connector_id, DisplayInfo& info) {
    drmModeConnector* connector = drmModeGetConnector(drm_fd_, connector_id);
    if (!connector) {
        return false;
    }
    
    info.connector_id = connector_id;
    info.connected = (connector->connection == DRM_MODE_CONNECTED);
    info.is_primary = false;
    
    // 生成连接器名称
    const char* connector_type_names[] = {
        "Unknown", "VGA", "DVII", "DVID", "DVIA", "Composite", "SVIDEO",
        "LVDS", "Component", "9PinDIN", "DisplayPort", "HDMIA", "HDMIB",
        "TV", "eDP", "VIRTUAL", "DSI", "DPI", "WRITEBACK", "SPI"
    };
    
    std::string type_name = "Unknown";
    if (connector->connector_type < sizeof(connector_type_names) / sizeof(connector_type_names[0])) {
        type_name = connector_type_names[connector->connector_type];
    }
    
    info.name = "card0-" + type_name + "-" + std::to_string(connector->connector_type_id);
    
    // 查找最佳模式
    drmModeModeInfo* best_mode = findBestMode(connector);
    if (best_mode) {
        info.mode = *best_mode;
        info.width = best_mode->hdisplay;
        info.height = best_mode->vdisplay;
    } else {
        info.width = 0;
        info.height = 0;
    }
    
    // 查找编码器和CRTC
    info.encoder_id = 0;
    info.crtc_id = 0;
    
    if (connector->encoder_id) {
        drmModeEncoder* encoder = drmModeGetEncoder(drm_fd_, connector->encoder_id);
        if (encoder) {
            info.encoder_id = encoder->encoder_id;
            info.crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
    }
    
    // 如果没有找到编码器，尝试查找可用的
    if (!info.crtc_id && connector->count_encoders > 0) {
        for (int i = 0; i < connector->count_encoders; i++) {
            drmModeEncoder* encoder = drmModeGetEncoder(drm_fd_, connector->encoders[i]);
            if (encoder) {
                // 查找空闲的CRTC
                for (int j = 0; j < resources_->count_crtcs; j++) {
                    if (encoder->possible_crtcs & (1 << j)) {
                        uint32_t crtc_id = resources_->crtcs[j];
                        // 检查CRTC是否已被使用
                        bool crtc_in_use = false;
                        for (const auto& existing : displays_) {
                            if (existing.crtc_id == crtc_id) {
                                crtc_in_use = true;
                                break;
                            }
                        }
                        if (!crtc_in_use) {
                            info.encoder_id = encoder->encoder_id;
                            info.crtc_id = crtc_id;
                            break;
                        }
                    }
                }
                drmModeFreeEncoder(encoder);
                if (info.crtc_id) break;
            }
        }
    }
    
    drmModeFreeConnector(connector);
    return true;
}

drmModeModeInfo* DRMManager::findBestMode(drmModeConnector* connector) {
    if (!connector || connector->count_modes == 0) {
        return nullptr;
    }
    
    // 选择首选模式，通常是第一个
    for (int i = 0; i < connector->count_modes; i++) {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            return &connector->modes[i];
        }
    }
    
    // 如果没有首选模式，选择分辨率最高的
    drmModeModeInfo* best = &connector->modes[0];
    for (int i = 1; i < connector->count_modes; i++) {
        if (connector->modes[i].hdisplay * connector->modes[i].vdisplay >
            best->hdisplay * best->vdisplay) {
            best = &connector->modes[i];
        }
    }
    
    return best;
}

DisplayInfo* DRMManager::getPrimaryDisplay() {
    for (auto& display : displays_) {
        if (display.is_primary) {
            return &display;
        }
    }
    return nullptr;
}

DisplayInfo* DRMManager::getDisplayByName(const std::string& name) {
    for (auto& display : displays_) {
        if (display.name == name) {
            return &display;
        }
    }
    return nullptr;
}

bool DRMManager::isDisplayConnected(const std::string& name) {
    // 重新扫描连接状态
    DisplayInfo* display = getDisplayByName(name);
    if (!display) return false;
    
    drmModeConnector* connector = drmModeGetConnector(drm_fd_, display->connector_id);
    if (!connector) return false;
    
    bool connected = (connector->connection == DRM_MODE_CONNECTED);
    display->connected = connected;
    
    drmModeFreeConnector(connector);
    return connected;
}

bool DRMManager::enableDisplay(DisplayInfo* display) {
    if (!display || !display->connected || !display->crtc_id) {
        return false;
    }
    
    std::cout << "Enabled display " << display->name << std::endl;
    return true;
}

bool DRMManager::disableDisplay(DisplayInfo* display) {
    if (!display || !display->crtc_id) {
        return false;
    }
    
    // 禁用CRTC
    int ret = drmModeSetCrtc(drm_fd_, display->crtc_id, 0, 0, 0, nullptr, 0, nullptr);
    
    if (ret) {
        std::cerr << "Failed to disable CRTC for display " << display->name 
                  << ": " << strerror(-ret) << std::endl;
        return false;
    }
    
    std::cout << "Disabled display " << display->name << std::endl;
    return true;
}

bool DRMManager::setCRTCWithFramebuffer(DisplayInfo* display, uint32_t fb_id) {
    if (!display || !display->connected || !display->crtc_id || !fb_id) {
        return false;
    }
    
    // 设置CRTC模式
    int ret = drmModeSetCrtc(drm_fd_, display->crtc_id, fb_id, 0, 0,
                            &display->connector_id, 1, &display->mode);
    
    if (ret) {
        std::cerr << "Failed to set CRTC for display " << display->name 
                  << ": " << strerror(-ret) << std::endl;
        return false;
    }
    
    return true;
}

uint32_t DRMManager::createFramebuffer(uint32_t width, uint32_t height, uint32_t format, 
                                      uint32_t handles[4], uint32_t pitches[4], uint32_t offsets[4]) {
    uint32_t fb_id;
    int ret = drmModeAddFB2(drm_fd_, width, height, format, handles, pitches, offsets, &fb_id, 0);
    
    if (ret) {
        std::cerr << "Failed to create framebuffer: " << strerror(-ret) << std::endl;
        return 0;
    }
    
    return fb_id;
}

void DRMManager::destroyFramebuffer(uint32_t fb_id) {
    if (fb_id) {
        drmModeRmFB(drm_fd_, fb_id);
    }
}

bool DRMManager::pageFlip(DisplayInfo* display, uint32_t fb_id) {
    if (!display || !display->crtc_id || !fb_id) {
        return false;
    }
    
    int ret = drmModePageFlip(drm_fd_, display->crtc_id, fb_id, 
                             DRM_MODE_PAGE_FLIP_EVENT, display);
    
    if (ret) {
        std::cerr << "Failed to page flip for display " << display->name 
                  << ": " << strerror(-ret) << std::endl;
        return false;
    }
    
    // 强制等待页面翻转完成
    fd_set fds;
    struct timeval timeout;
    
    FD_ZERO(&fds);
    FD_SET(drm_fd_, &fds);
    
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;  // 50ms timeout
    
    int select_ret = select(drm_fd_ + 1, &fds, nullptr, nullptr, &timeout);
    if (select_ret > 0 && FD_ISSET(drm_fd_, &fds)) {
        drmEventContext evctx = {};
        evctx.version = 2;
        evctx.page_flip_handler = [](int fd, unsigned int frame, unsigned int sec, 
                                   unsigned int usec, void* data) {
            // Page flip completed
        };
        
        drmHandleEvent(drm_fd_, &evctx);
    }
    
    return true;
}

bool DRMManager::waitForPageFlipEvents(int timeout_ms) {
    fd_set fds;
    struct timeval timeout;
    
    FD_ZERO(&fds);
    FD_SET(drm_fd_, &fds);
    
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    int ret = select(drm_fd_ + 1, &fds, nullptr, nullptr, &timeout);
    if (ret <= 0) {
        return false;  // Timeout or error
    }
    
    if (FD_ISSET(drm_fd_, &fds)) {
        drmEventContext evctx = {};
        evctx.version = 2;
        evctx.page_flip_handler = [](int fd, unsigned int frame, unsigned int sec, 
                                   unsigned int usec, void* data) {
            // Page flip completed - buffer is now free for reuse
        };
        
        drmHandleEvent(drm_fd_, &evctx);
        return true;
    }
    
    return false;
}

void DRMManager::handlePageFlipEvent(unsigned int frame, unsigned int sec, unsigned int usec, void* data) {
    // This callback is called when page flip completes
    // The buffer used in the page flip is now free for reuse
} 