#pragma once

#include <cstdint>
#include <memory>

// RGA IM2D API 相关的结构体和函数声明
#ifdef HAVE_RGA
#include <rga/im2d.hpp>
#include <rga/im2d_type.h>
#else
// Stub definitions for when RGA IM2D is not available
#include <rga/im2d_type.h>

// 简化的stub实现，仅声明函数
IM_STATUS imresize(const rga_buffer_t& src, rga_buffer_t& dst);
IM_STATUS imcopy(const rga_buffer_t& src, rga_buffer_t& dst);
IM_STATUS imrotate(const rga_buffer_t& src, rga_buffer_t& dst, int mode);
rga_buffer_handle_t importbuffer_fd(int fd, int width, int height, int format);
rga_buffer_handle_t importbuffer_virtualaddr(void* va, int width, int height, int format);
IM_STATUS releasebuffer_handle(rga_buffer_handle_t handle);
rga_buffer_t wrapbuffer_handle(rga_buffer_handle_t handle, int width, int height, int format);
#endif

struct FrameBuffer {
    void* virtual_addr;
    uint32_t physical_addr;
    int dma_fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t size;
};

class RGAHelper {
public:
    RGAHelper();
    ~RGAHelper();
    
    bool initialize();
    void cleanup();
    
    // 缩放和格式转换 - 使用IM2D API
    bool scaleAndCopy(const FrameBuffer& src, FrameBuffer& dst, 
                     uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h,
                     uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
                     int rotation_degrees = 0);
    
    // 简单复制
    bool copy(const FrameBuffer& src, FrameBuffer& dst);
    
    // 分配DMA缓冲区
    bool allocateBuffer(FrameBuffer& buffer, uint32_t width, uint32_t height, uint32_t format);
    void freeBuffer(FrameBuffer& buffer);
    
    // 格式转换
    uint32_t drmFormatToRgaFormat(uint32_t drm_format);
    
private:
    bool rga_initialized_;
    
    // 创建RGA buffer
    rga_buffer_t createRgaBuffer(const FrameBuffer& fb);
    rga_buffer_t createRgaBuffer(const FrameBuffer& fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
}; 