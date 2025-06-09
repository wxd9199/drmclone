#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <vector>
#include <string>
#include <memory>

struct DisplayInfo {
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t encoder_id;
    drmModeModeInfo mode;
    std::string name;
    bool connected;
    bool is_primary;
    uint32_t width;
    uint32_t height;
};

class DRMManager {
public:
    DRMManager();
    ~DRMManager();
    
    bool initialize(const char* device_path = "/dev/dri/card0");
    void cleanup();
    
    bool scanDisplays();
    std::vector<DisplayInfo> getDisplays() const { return displays_; }
    DisplayInfo* getPrimaryDisplay();
    DisplayInfo* getDisplayByName(const std::string& name);
    
    bool isDisplayConnected(const std::string& name);
    bool enableDisplay(DisplayInfo* display);
    bool disableDisplay(DisplayInfo* display);
    bool setCRTCWithFramebuffer(DisplayInfo* display, uint32_t fb_id);
    
    // Framebuffer operations
    uint32_t createFramebuffer(uint32_t width, uint32_t height, uint32_t format, uint32_t handles[4], uint32_t pitches[4], uint32_t offsets[4]);
    void destroyFramebuffer(uint32_t fb_id);
    
    // Page flip operations
    bool pageFlip(DisplayInfo* display, uint32_t fb_id);
    
    // Page flip event handling
    bool waitForPageFlipEvents(int timeout_ms = 16);
    void handlePageFlipEvent(unsigned int frame, unsigned int sec, unsigned int usec, void* data);
    
    int getFd() const { return drm_fd_; }
    
private:
    int drm_fd_;
    drmModeRes* resources_;
    std::vector<DisplayInfo> displays_;
    
    bool probeDrmDevice();
    bool getConnectorInfo(uint32_t connector_id, DisplayInfo& info);
    drmModeModeInfo* findBestMode(drmModeConnector* connector);
}; 