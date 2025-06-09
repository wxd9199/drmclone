#pragma once

#include "drm_manager.h"
#include "rga_helper.h"
#include <memory>
#include <map>
#include <gbm.h>

struct GBMBuffer {
    struct gbm_bo* bo;
    uint32_t fb_id;
    FrameBuffer frame_buffer;
    bool valid;
};

// 配置选项
struct DisplayConfig {
    enum ScaleMode {
        SCALE_STRETCH,      // 拉伸铺满全屏
        SCALE_KEEP_ASPECT   // 保持宽高比
    };
    
    enum Quality {
        QUALITY_FAST,       // 最近邻插值，高性能
        QUALITY_GOOD        // 双线性插值，高质量
    };
    
    ScaleMode scale_mode = SCALE_STRETCH;
    int rotation_degrees = 90;         // 旋转角度：0, 90, 180, 270
    Quality quality = QUALITY_GOOD;    // 默认使用好质量
    bool enable_debug = false;
};

class FrameCopier {
public:
    FrameCopier(std::shared_ptr<DRMManager> drm_manager, std::shared_ptr<RGAHelper> rga_helper);
    ~FrameCopier();
    
    bool initialize();
    void cleanup();
    
    // 从主显示器获取当前帧
    bool captureFrame(DisplayInfo* primary_display, FrameBuffer& frame);
    
    // 复制帧到目标显示器，并自适应分辨率
    bool copyToDisplay(const FrameBuffer& source_frame, DisplayInfo* target_display);
    
    // 配置管理
    void setConfig(const DisplayConfig& config) { config_ = config; }
    const DisplayConfig& getConfig() const { return config_; }
    
    // 为显示器创建缓冲区
    bool createBuffersForDisplay(DisplayInfo* display);
    void destroyBuffersForDisplay(DisplayInfo* display);
    
    // 获取当前缓冲区
    GBMBuffer* getCurrentBuffer(DisplayInfo* display);
    
private:
    std::shared_ptr<DRMManager> drm_manager_;
    std::shared_ptr<RGAHelper> rga_helper_;
    
    struct gbm_device* gbm_device_;
    std::map<uint32_t, std::vector<GBMBuffer>> display_buffers_;  // connector_id -> buffers
    std::map<uint32_t, int> current_buffer_index_;  // connector_id -> current buffer index
    
    DisplayConfig config_;  // 显示配置
    
    bool setupGBM();
    GBMBuffer* getNextBuffer(DisplayInfo* display);
    
    // 计算最佳缩放参数
    void calculateScaling(uint32_t src_width, uint32_t src_height,
                         uint32_t dst_width, uint32_t dst_height,
                         uint32_t& scale_x, uint32_t& scale_y,
                         uint32_t& scale_width, uint32_t& scale_height);
    
    // 带配置的图像变换
    void copyWithTransform(uint32_t* src_pixels, uint32_t* dst_pixels,
                          uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                          int rotation, DisplayConfig::ScaleMode scale_mode, 
                          DisplayConfig::Quality quality);
}; 