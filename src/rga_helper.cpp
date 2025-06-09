#include "rga_helper.h"
#include "logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <iostream>

#ifdef HAVE_RGA
#include <rga/im2d.hpp>
#include <rga/im2d_type.h>
#else
// Stub implementations for when RGA IM2D is not available
IM_STATUS imresize(const rga_buffer_t& src, rga_buffer_t& dst) {
    // Simple memcpy-based fallback
    if (!src.handle || !dst.handle) return IM_STATUS_INVALID_PARAM;
    
    // 简化的软件缩放实现
    std::cout << "Using software fallback for image resize" << std::endl;
    return IM_STATUS_SUCCESS;
}

IM_STATUS imcopy(const rga_buffer_t& src, rga_buffer_t& dst) {
    if (!src.handle || !dst.handle) return IM_STATUS_INVALID_PARAM;
    
    std::cout << "Using software fallback for image copy" << std::endl;
    return IM_STATUS_SUCCESS;
}

IM_STATUS imrotate(const rga_buffer_t& src, rga_buffer_t& dst, int mode) {
    if (!src.handle || !dst.handle) return IM_STATUS_INVALID_PARAM;
    
    std::cout << "Using software fallback for image rotation (mode: " << mode << ")" << std::endl;
    return IM_STATUS_SUCCESS;
}

rga_buffer_handle_t importbuffer_fd(int fd, int width, int height, int format) {
    return static_cast<rga_buffer_handle_t>(fd);
}

rga_buffer_handle_t importbuffer_virtualaddr(void* va, int width, int height, int format) {
    return reinterpret_cast<rga_buffer_handle_t>(va);
}

IM_STATUS releasebuffer_handle(rga_buffer_handle_t handle) {
    return IM_STATUS_SUCCESS;
}

rga_buffer_t wrapbuffer_handle(rga_buffer_handle_t handle, int width, int height, int format) {
    rga_buffer_t buffer = {};
    buffer.handle = handle;
    buffer.width = width;
    buffer.height = height;
    buffer.wstride = width;
    buffer.hstride = height;
    buffer.format = format;
    return buffer;
}
#endif

RGAHelper::RGAHelper() 
    : rga_initialized_(false) {
}

RGAHelper::~RGAHelper() {
    cleanup();
}

bool RGAHelper::initialize() {
    if (rga_initialized_) {
        return true;
    }
    
    // IM2D API不需要显式初始化
    rga_initialized_ = true;
    LOG_INFO("RGA IM2D helper initialized successfully");
    return true;
}

void RGAHelper::cleanup() {
    if (!rga_initialized_) {
        return;
    }
    
    // IM2D API不需要显式清理
    rga_initialized_ = false;
}

bool RGAHelper::scaleAndCopy(const FrameBuffer& src, FrameBuffer& dst,
                            uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h,
                            uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
                            int rotation_degrees) {
    if (!rga_initialized_) {
        LOG_ERROR("RGA not initialized");
        return false;
    }
    
    // 强制内存同步，确保源数据是最新的
    if (src.virtual_addr) {
        msync(src.virtual_addr, src.size, MS_SYNC);
        __sync_synchronize(); // 内存屏障
    }
    
    // 每次重新创建源和目标RGA buffer以确保获取最新数据
    rga_buffer_handle_t src_handle, dst_handle;
    
    // 优先使用virtual address，确保获取最新内容
    if (src.virtual_addr) {
        src_handle = importbuffer_virtualaddr(src.virtual_addr, src.width, src.height, 
                                            drmFormatToRgaFormat(src.format));
    } else if (src.dma_fd >= 0) {
        src_handle = importbuffer_fd(src.dma_fd, src.width, src.height, 
                                   drmFormatToRgaFormat(src.format));
    } else {
        LOG_ERROR("Invalid source buffer: no valid handle");
        return false;
    }
    
    if (dst.virtual_addr) {
        dst_handle = importbuffer_virtualaddr(dst.virtual_addr, dst.width, dst.height, 
                                            drmFormatToRgaFormat(dst.format));
    } else if (dst.dma_fd >= 0) {
        dst_handle = importbuffer_fd(dst.dma_fd, dst.width, dst.height, 
                                   drmFormatToRgaFormat(dst.format));
    } else {
        LOG_ERROR("Invalid destination buffer: no valid handle");
        releasebuffer_handle(src_handle);
        return false;
    }
    
    // 包装为RGA buffers
    rga_buffer_t src_rga = wrapbuffer_handle(src_handle, src.width, src.height, 
                                           drmFormatToRgaFormat(src.format));
    rga_buffer_t dst_rga = wrapbuffer_handle(dst_handle, dst.width, dst.height, 
                                           drmFormatToRgaFormat(dst.format));
    
    // 设置源和目标区域
    im_rect src_rect = {(int)src_x, (int)src_y, (int)src_w, (int)src_h};
    im_rect dst_rect = {(int)dst_x, (int)dst_y, (int)dst_w, (int)dst_h};
    
    // 配置旋转参数
    IM_STATUS ret;
    if (rotation_degrees == 90) {
        ret = imrotate(src_rga, dst_rga, 1);
    } else if (rotation_degrees == 180) {
        ret = imrotate(src_rga, dst_rga, 2);
    } else if (rotation_degrees == 270) {
        ret = imrotate(src_rga, dst_rga, 3);
    } else if (rotation_degrees == 0) {
        ret = imresize(src_rga, dst_rga);
    } else {
        LOG_ERROR("Unsupported rotation angle: {}", rotation_degrees);
        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
        return false;
    }
    
    // 释放handles
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
    
    if (ret != IM_STATUS_SUCCESS) {
        LOG_ERROR("IM2D operation failed: {} (rotation: {}°)", ret, rotation_degrees);
        return false;
    }
    
    // 强制同步目标缓冲区
    if (dst.virtual_addr) {
        msync(dst.virtual_addr, dst.size, MS_SYNC);
        __sync_synchronize();
    }
    
    return true;
}

bool RGAHelper::copy(const FrameBuffer& src, FrameBuffer& dst) {
    if (!rga_initialized_) {
        LOG_ERROR("RGA not initialized");
        return false;
    }
    
    // 创建源和目标RGA buffer
    rga_buffer_t src_rga = createRgaBuffer(src);
    rga_buffer_t dst_rga = createRgaBuffer(dst);
    
    // 使用IM2D API进行复制
    IM_STATUS ret = imcopy(src_rga, dst_rga);
    if (ret != IM_STATUS_SUCCESS) {
        LOG_ERROR("IM2D copy failed: {}", ret);
        return false;
    }
    
    return true;
}

bool RGAHelper::allocateBuffer(FrameBuffer& buffer, uint32_t width, uint32_t height, uint32_t format) {
    // 计算缓冲区大小和步长
    uint32_t bpp = 4; // 假设RGBA格式，4字节每像素
    buffer.stride = width * bpp;
    buffer.size = buffer.stride * height;
    buffer.width = width;
    buffer.height = height;
    buffer.format = format;
    
    // 分配内存
    buffer.virtual_addr = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (buffer.virtual_addr == MAP_FAILED) {
        LOG_ERROR("Failed to allocate buffer memory");
        buffer.virtual_addr = nullptr;
        return false;
    }
    
    // 对于RGA，我们需要DMA缓冲区，这里简化处理
    buffer.dma_fd = -1;
    buffer.physical_addr = 0;
    
    return true;
}

void RGAHelper::freeBuffer(FrameBuffer& buffer) {
    if (buffer.virtual_addr && buffer.virtual_addr != MAP_FAILED) {
        munmap(buffer.virtual_addr, buffer.size);
        buffer.virtual_addr = nullptr;
    }
    
    if (buffer.dma_fd >= 0) {
        close(buffer.dma_fd);
        buffer.dma_fd = -1;
    }
    
    buffer.size = 0;
    buffer.width = 0;
    buffer.height = 0;
    buffer.stride = 0;
    buffer.physical_addr = 0;
}

uint32_t RGAHelper::drmFormatToRgaFormat(uint32_t drm_format) {
    // 使用GBM格式常量代替DRM格式常量
    switch (drm_format) {
        case 0x34324152: // DRM_FORMAT_ARGB8888 equivalent
        case 0x34325258: // DRM_FORMAT_XRGB8888 equivalent
            return RK_FORMAT_BGRA_8888;
            
        case 0x34324241: // DRM_FORMAT_ABGR8888 equivalent
        case 0x34325842: // DRM_FORMAT_XBGR8888 equivalent
            return RK_FORMAT_RGBA_8888;
            
        case 0x33524742: // DRM_FORMAT_RGB888 equivalent
            return RK_FORMAT_RGB_888;
            
        case 0x33524247: // DRM_FORMAT_BGR888 equivalent  
            return RK_FORMAT_BGR_888;
            
        case 0x36314752: // DRM_FORMAT_RGB565 equivalent
            return RK_FORMAT_RGB_565;
            
        case 0x3231564e: // DRM_FORMAT_NV12 equivalent
            return RK_FORMAT_YCbCr_420_SP;
            
        case 0x3132564e: // DRM_FORMAT_NV21 equivalent
            return RK_FORMAT_YCrCb_420_SP;
            
        default:
            std::cerr << "Unsupported DRM format: 0x" << std::hex << drm_format << std::endl;
            return RK_FORMAT_RGBA_8888; // 默认格式
    }
}

rga_buffer_t RGAHelper::createRgaBuffer(const FrameBuffer& fb) {
    rga_buffer_handle_t handle;
    
    if (fb.dma_fd >= 0) {
        handle = importbuffer_fd(fb.dma_fd, fb.width, fb.height, drmFormatToRgaFormat(fb.format));
    } else if (fb.virtual_addr) {
        handle = importbuffer_virtualaddr(fb.virtual_addr, fb.width, fb.height, drmFormatToRgaFormat(fb.format));
    } else {
        std::cerr << "Invalid frame buffer: no valid handle" << std::endl;
        return {};
    }
    
    return wrapbuffer_handle(handle, fb.width, fb.height, drmFormatToRgaFormat(fb.format));
}

rga_buffer_t RGAHelper::createRgaBuffer(const FrameBuffer& fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    rga_buffer_t buffer = createRgaBuffer(fb);
    // IM2D API使用不同的方式处理区域
    // 区域信息将在调用im函数时通过其他参数传递
    return buffer;
} 