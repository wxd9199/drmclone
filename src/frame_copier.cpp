#include "frame_copier.h"
#include "logger.h"
#include <drm_fourcc.h>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <algorithm>
#include <sys/mman.h>
#include <errno.h>
#include <chrono>
#include <cmath>

FrameCopier::FrameCopier(std::shared_ptr<DRMManager> drm_manager, 
                         std::shared_ptr<RGAHelper> rga_helper)
    : drm_manager_(drm_manager), rga_helper_(rga_helper), gbm_device_(nullptr) {
}

FrameCopier::~FrameCopier() {
    cleanup();
}

bool FrameCopier::initialize() {
    if (!drm_manager_ || !rga_helper_) {
        LOG_ERROR("DRM manager or RGA helper not available");
        return false;
    }
    
    if (!setupGBM()) {
        LOG_ERROR("Failed to setup GBM");
        return false;
    }
    
    LOG_INFO("Frame copier initialized successfully");
    return true;
}

void FrameCopier::cleanup() {
    // 清理所有显示器的缓冲区
    for (auto& [connector_id, buffers] : display_buffers_) {
        for (auto& buffer : buffers) {
            if (buffer.fb_id) {
                drm_manager_->destroyFramebuffer(buffer.fb_id);
            }
            if (buffer.bo) {
                gbm_bo_destroy(buffer.bo);
            }
            if (buffer.frame_buffer.virtual_addr) {
                rga_helper_->freeBuffer(buffer.frame_buffer);
            }
        }
    }
    display_buffers_.clear();
    current_buffer_index_.clear();
    
    if (gbm_device_) {
        gbm_device_destroy(gbm_device_);
        gbm_device_ = nullptr;
    }
}

bool FrameCopier::setupGBM() {
    int drm_fd = drm_manager_->getFd();
    if (drm_fd < 0) {
        return false;
    }
    
    gbm_device_ = gbm_create_device(drm_fd);
    if (!gbm_device_) {
        LOG_ERROR("Failed to create GBM device");
        return false;
    }
    
    return true;
}

bool FrameCopier::captureFrame(DisplayInfo* primary_display, FrameBuffer& frame) {
    if (!primary_display || !primary_display->connected) {
        return false;
    }
    
    uint32_t width = primary_display->width;
    uint32_t height = primary_display->height;
    uint32_t format = DRM_FORMAT_XRGB8888;
    
    if (!rga_helper_->allocateBuffer(frame, width, height, format)) {
        return false;
    }
    
    bool captured_real_content = false;
    static uint32_t last_pixel_checksum = 0;
    
    // 等待vblank以确保framebuffer是最新的
    if (primary_display->crtc_id) {
        drmVBlank vbl;
        vbl.request.type = DRM_VBLANK_RELATIVE;
        vbl.request.sequence = 1;
        vbl.request.signal = 0;
        drmWaitVBlank(drm_manager_->getFd(), &vbl);
    }
    
    // 尝试读取当前活跃的framebuffer
    if (primary_display->crtc_id && frame.virtual_addr) {
        drmModeCrtc* crtc = drmModeGetCrtc(drm_manager_->getFd(), primary_display->crtc_id);
        if (crtc && crtc->buffer_id) {
            drmModeFB* fb = drmModeGetFB(drm_manager_->getFd(), crtc->buffer_id);
            if (fb && fb->handle) {
                // 尝试多次读取，确保获取最新内容
                bool mapping_success = false;
                for (int retry = 0; retry < 3 && !mapping_success; retry++) {
                    struct drm_mode_map_dumb map_req = {};
                    map_req.handle = fb->handle;
                    
                    if (drmIoctl(drm_manager_->getFd(), DRM_IOCTL_MODE_MAP_DUMB, &map_req) == 0) {
                        void* fb_ptr = mmap(0, fb->height * fb->pitch, PROT_READ, MAP_SHARED,
                                           drm_manager_->getFd(), map_req.offset);
                        
                        if (fb_ptr != MAP_FAILED) {
                            // 强制同步内存，确保读取最新数据
                            msync(fb_ptr, fb->height * fb->pitch, MS_SYNC);
                            
                            // 添加内存屏障，强制刷新CPU缓存
                            __sync_synchronize();
                            
                            uint32_t* src_pixels = (uint32_t*)fb_ptr;
                            uint32_t* dst_pixels = (uint32_t*)frame.virtual_addr;
                            
                            // 计算有效复制区域
                            uint32_t copy_width = std::min(width, fb->width);
                            uint32_t copy_height = std::min(height, fb->height);
                            uint32_t src_stride_pixels = fb->pitch / 4;
                            uint32_t dst_stride_pixels = frame.stride / 4;
                            
                            // 逐行复制，处理步长差异
                            for (uint32_t y = 0; y < copy_height; y++) {
                                memcpy(&dst_pixels[y * dst_stride_pixels],
                                      &src_pixels[y * src_stride_pixels],
                                      copy_width * 4);
                            }
                            

                            
                            // 计算像素内容的简单校验和来检测变化
                            uint32_t checksum = 0;
                            for (uint32_t i = 0; i < std::min(1000u, copy_width * copy_height); i++) {
                                checksum ^= dst_pixels[i];
                            }
                            
                            captured_real_content = true;
                            mapping_success = true;
                            
                            // 检测内容变化 - 仅在严格调试模式下记录
                            static int success_count = 0;
                            static uint32_t frame_checksums[10] = {0}; // 记录最近10帧的校验和
                            static auto last_report_time = std::chrono::steady_clock::now();
                            static bool first_capture_logged = false;
                            ++success_count;
                            
                            bool content_changed = (checksum != last_pixel_checksum);
                            frame_checksums[success_count % 10] = checksum;
                            
                            // 只在第一次成功capture时记录一次，之后不再循环记录
                            if (!first_capture_logged) {
                                LOG_INFO("DSI capture started successfully, frame mirroring active");
                                first_capture_logged = true;
                            }
                            
                            // 仅在每300帧（10秒）或严重错误时记录
                            auto now = std::chrono::steady_clock::now();
                            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report_time);
                            
                            if (success_count % 300 == 0 && elapsed.count() >= 10) {
                                // 检查最近10帧是否有任何变化
                                bool any_variation = false;
                                for (int i = 1; i < 10; i++) {
                                    if (frame_checksums[i] != frame_checksums[0]) {
                                        any_variation = true;
                                        break;
                                    }
                                }
                                
                                // 只在没有变化（可能有问题）时记录
                                if (!any_variation) {
                                    LOG_WARN("DSI capture: No content variation detected in last 10 frames");
                                }
                                last_report_time = now;
                            }
                            
                            last_pixel_checksum = checksum;
                            
                            munmap(fb_ptr, fb->height * fb->pitch);
                        } else {
                            // 映射失败，短暂等待后重试
                            usleep(1000); // 1ms
                        }
                    } else {
                        usleep(1000); // 1ms
                    }
                }
                drmModeFreeFB(fb);
            }
            drmModeFreeCrtc(crtc);
        }
    }
    
    // 如果无法读取真实内容，使用简单的色块作为后备
    if (!captured_real_content && frame.virtual_addr) {
        uint32_t* pixels = (uint32_t*)frame.virtual_addr;
        
        // 简单的渐变填充，便于识别是否为后备内容
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t gray = (x + y) % 256;
                pixels[y * width + x] = (gray << 16) | (gray << 8) | gray;
            }
        }
        
        static int fallback_count = 0;
        if (++fallback_count % 30 == 0) {  // 更频繁地报告
            std::cout << "DSI framebuffer capture FAILED - using fallback pattern (attempt " 
                     << fallback_count << ")" << std::endl;
        }
    }
    
    return true;
}

bool FrameCopier::copyToDisplay(const FrameBuffer& source_frame, DisplayInfo* target_display) {
    if (!target_display || !target_display->connected) {
        return false;
    }
    
    GBMBuffer* target_buffer = getNextBuffer(target_display);
    if (!target_buffer || !target_buffer->valid) {
        std::cerr << "Failed to get target buffer for " << target_display->name << std::endl;
        return false;
    }
    
    // 使用static map为每个显示器单独计数
    static std::map<std::string, int> copy_counts;
    copy_counts[target_display->name]++;
    
    // if (copy_counts[target_display->name] % 60 == 0) {
    //     // // std::cout << "Copying frame " << copy_counts[target_display->name] << " to " << target_display->name 
    //     //           << " (source: " << source_frame.width << "x" << source_frame.height 
    //     //           << ", target: " << target_display->width << "x" << target_display->height << ")" << std::endl;
    // }
    
    uint32_t scale_x, scale_y, scale_width, scale_height;
    calculateScaling(source_frame.width, source_frame.height,
                    target_display->width, target_display->height,
                    scale_x, scale_y, scale_width, scale_height);
    
    // 使用配置中的旋转角度
    int rotation_degrees = config_.rotation_degrees;
    
    // if (copy_counts[target_display->name] % 60 == 0) {
    //     std::cout << "  Scaling to: " << scale_width << "x" << scale_height 
    //               << " at offset (" << scale_x << "," << scale_y 
    //               << "), rotation: " << rotation_degrees << "°" << std::endl;
    // }
    
    // 优先使用RGA硬件加速，仅在失败时使用CPU复制
    bool use_cpu_copy = false;  // 优先使用RGA提高性能
    bool success = false;
    
    if (!use_cpu_copy) {
        // 确保源缓冲区数据是最新的
        if (source_frame.virtual_addr) {
            // 强制刷新源缓冲区缓存
            msync(source_frame.virtual_addr, source_frame.size, MS_SYNC);
            __sync_synchronize();
        }
        
        // 使用RGA硬件加速
        success = rga_helper_->scaleAndCopy(
            source_frame, target_buffer->frame_buffer,
            0, 0, source_frame.width, source_frame.height,
            scale_x, scale_y, scale_width, scale_height,
            rotation_degrees
        );
        
        if (!success) {
            LOG_WARN("RGA copy failed for {}, falling back to CPU copy", target_display->name);
            use_cpu_copy = true;
        }
    }
    
    if (use_cpu_copy) {
        // CPU直接复制和旋转
        // if (copy_counts[target_display->name] % 60 == 0) {
        //     std::cout << "  Using CPU copy instead of RGA..." << std::endl;
        // }
        
        // 获取目标缓冲区的虚拟地址
        void* target_addr = nullptr;
        uint32_t stride;
        void* map_data;
        if (target_buffer->bo) {
            target_addr = gbm_bo_map(target_buffer->bo, 0, 0, 
                                   target_display->width, target_display->height,
                                   GBM_BO_TRANSFER_WRITE, &stride, &map_data);
        }
        
        if (target_addr && source_frame.virtual_addr) {
            uint32_t* src_pixels = (uint32_t*)source_frame.virtual_addr;
            uint32_t* dst_pixels = (uint32_t*)target_addr;
            
            // 简单的缩放和旋转（90度顺时针）
            uint32_t src_w = source_frame.width;
            uint32_t src_h = source_frame.height;
            uint32_t dst_w = target_display->width;
            uint32_t dst_h = target_display->height;
            
            // 清空目标缓冲区
            memset(dst_pixels, 0, dst_w * dst_h * 4);
            
            // 根据配置进行缩放和旋转
            copyWithTransform(src_pixels, dst_pixels, src_w, src_h, dst_w, dst_h, 
                            rotation_degrees, config_.scale_mode, config_.quality);
            
            gbm_bo_unmap(target_buffer->bo, map_data);
            success = true;
        }
    } else {
        // 原来的RGA方法
        success = rga_helper_->scaleAndCopy(
            source_frame, target_buffer->frame_buffer,
            0, 0, source_frame.width, source_frame.height,
            scale_x, scale_y, scale_width, scale_height,
            rotation_degrees
        );
    }
    
        // 使用RGA硬件加速复制DSI内容到目标显示器
    // 移除循环打印
    
    if (!success) {
        LOG_ERROR("Failed to scale and copy frame to {}", target_display->name);
        return false;
    }
    
    // 强制同步DMA缓冲区，确保数据写入到显示硬件
    if (target_buffer->frame_buffer.dma_fd >= 0) {
        fsync(target_buffer->frame_buffer.dma_fd);
    }
    
    // 添加内存屏障确保所有写操作完成
    __sync_synchronize();
    
    // 移除循环打印
    
    // 使用pageFlip确保垂直同步
    success = drm_manager_->pageFlip(target_display, target_buffer->fb_id);
    if (!success) {
        LOG_ERROR("Failed to page flip for {}", target_display->name);
        return false;
    }
    
    // 移除循环打印
    
    current_buffer_index_[target_display->connector_id] = 
        (current_buffer_index_[target_display->connector_id] + 1) % 2;
    
    return true;
}

bool FrameCopier::createBuffersForDisplay(DisplayInfo* display) {
    if (!display || !gbm_device_) {
        return false;
    }
    
    uint32_t connector_id = display->connector_id;
    uint32_t width = display->width;
    uint32_t height = display->height;
    uint32_t format = GBM_FORMAT_XRGB8888;
    
    std::vector<GBMBuffer> buffers(2);
    
    for (int i = 0; i < 2; i++) {
        GBMBuffer& buffer = buffers[i];
        
        buffer.bo = gbm_bo_create(gbm_device_, width, height, format, 
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        
        if (!buffer.bo) {
            LOG_ERROR("Failed to create GBM BO for {}", display->name);
            
            for (int j = 0; j < i; j++) {
                if (buffers[j].fb_id) {
                    drm_manager_->destroyFramebuffer(buffers[j].fb_id);
                }
                if (buffers[j].bo) {
                    gbm_bo_destroy(buffers[j].bo);
                }
            }
            return false;
        }
        
        uint32_t bo_width = gbm_bo_get_width(buffer.bo);
        uint32_t bo_height = gbm_bo_get_height(buffer.bo);
        uint32_t bo_stride = gbm_bo_get_stride(buffer.bo);
        uint32_t bo_handle = gbm_bo_get_handle(buffer.bo).u32;
        uint32_t bo_format = gbm_bo_get_format(buffer.bo);
        
        uint32_t handles[4] = {bo_handle, 0, 0, 0};
        uint32_t pitches[4] = {bo_stride, 0, 0, 0};
        uint32_t offsets[4] = {0, 0, 0, 0};
        
        buffer.fb_id = drm_manager_->createFramebuffer(bo_width, bo_height, bo_format,
                                                      handles, pitches, offsets);
        
        if (!buffer.fb_id) {
            LOG_ERROR("Failed to create framebuffer for {}", display->name);
            
            gbm_bo_destroy(buffer.bo);
            for (int j = 0; j < i; j++) {
                if (buffers[j].fb_id) {
                    drm_manager_->destroyFramebuffer(buffers[j].fb_id);
                }
                if (buffers[j].bo) {
                    gbm_bo_destroy(buffers[j].bo);
                }
            }
            return false;
        }
        
        buffer.frame_buffer.width = bo_width;
        buffer.frame_buffer.height = bo_height;
        buffer.frame_buffer.stride = bo_stride;
        buffer.frame_buffer.format = bo_format;
        buffer.frame_buffer.size = bo_stride * bo_height;
        buffer.frame_buffer.dma_fd = gbm_bo_get_fd(buffer.bo);
        buffer.frame_buffer.virtual_addr = nullptr;
        buffer.frame_buffer.physical_addr = 0;
        
        buffer.valid = true;
    }
    
    display_buffers_[connector_id] = std::move(buffers);
    current_buffer_index_[connector_id] = 0;
    
    LOG_INFO("Created buffers for display {}", display->name);
    return true;
}

void FrameCopier::destroyBuffersForDisplay(DisplayInfo* display) {
    if (!display) {
        return;
    }
    
    uint32_t connector_id = display->connector_id;
    auto it = display_buffers_.find(connector_id);
    
    if (it != display_buffers_.end()) {
        for (auto& buffer : it->second) {
            if (buffer.fb_id) {
                drm_manager_->destroyFramebuffer(buffer.fb_id);
            }
            if (buffer.bo) {
                gbm_bo_destroy(buffer.bo);
            }
            if (buffer.frame_buffer.dma_fd >= 0) {
                close(buffer.frame_buffer.dma_fd);
            }
        }
        
        display_buffers_.erase(it);
        current_buffer_index_.erase(connector_id);
        
        LOG_INFO("Destroyed buffers for display {}", display->name);
    }
}

GBMBuffer* FrameCopier::getCurrentBuffer(DisplayInfo* display) {
    if (!display) {
        return nullptr;
    }
    
    uint32_t connector_id = display->connector_id;
    auto it = display_buffers_.find(connector_id);
    
    if (it == display_buffers_.end()) {
        return nullptr;
    }
    
    int index = current_buffer_index_[connector_id];
    return &it->second[index];
}

GBMBuffer* FrameCopier::getNextBuffer(DisplayInfo* display) {
    if (!display) {
        return nullptr;
    }
    
    uint32_t connector_id = display->connector_id;
    auto it = display_buffers_.find(connector_id);
    
    if (it == display_buffers_.end()) {
        LOG_WARN("No buffers found for {}, attempting to recreate...", display->name);
        if (createBuffersForDisplay(display)) {
            it = display_buffers_.find(connector_id);
        } else {
            return nullptr;
        }
    }
    
    int next_index = (current_buffer_index_[connector_id] + 1) % 2;
    GBMBuffer* buffer = &it->second[next_index];
    
    // 检查缓冲区有效性
    if (!buffer->valid || !buffer->bo || !buffer->fb_id) {
        LOG_WARN("Invalid buffer for {}, attempting to recreate...", display->name);
        destroyBuffersForDisplay(display);
        if (createBuffersForDisplay(display)) {
            it = display_buffers_.find(connector_id);
            buffer = &it->second[next_index];
        } else {
            return nullptr;
        }
    }
    
    return buffer;
}

void FrameCopier::calculateScaling(uint32_t src_width, uint32_t src_height,
                                  uint32_t dst_width, uint32_t dst_height,
                                  uint32_t& scale_x, uint32_t& scale_y,
                                  uint32_t& scale_width, uint32_t& scale_height) {
    float scale_factor_x = (float)dst_width / src_width;
    float scale_factor_y = (float)dst_height / src_height;
    
    float scale_factor = std::min(scale_factor_x, scale_factor_y);
    
    scale_width = (uint32_t)(src_width * scale_factor);
    scale_height = (uint32_t)(src_height * scale_factor);
    
    scale_x = (dst_width - scale_width) / 2;
    scale_y = (dst_height - scale_height) / 2;
}

void FrameCopier::copyWithTransform(uint32_t* src_pixels, uint32_t* dst_pixels,
                                   uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                                   int rotation, DisplayConfig::ScaleMode scale_mode, 
                                   DisplayConfig::Quality quality) {
    
    // 处理旋转后的有效尺寸
    uint32_t effective_src_w = src_w;
    uint32_t effective_src_h = src_h;
    if (rotation == 90 || rotation == 270) {
        std::swap(effective_src_w, effective_src_h);
    }
    
    // 计算缩放参数
    uint32_t scaled_w, scaled_h, offset_x = 0, offset_y = 0;
    
    if (scale_mode == DisplayConfig::SCALE_STRETCH) {
        // 拉伸模式：铺满全屏
        scaled_w = dst_w;
        scaled_h = dst_h;
    } else {
        // 保持宽高比模式
        float scale_factor = std::min((float)dst_w / effective_src_w, (float)dst_h / effective_src_h);
        scaled_w = (uint32_t)(effective_src_w * scale_factor);
        scaled_h = (uint32_t)(effective_src_h * scale_factor);
        offset_x = (dst_w - scaled_w) / 2;
        offset_y = (dst_h - scaled_h) / 2;
    }
    
    // 对90度旋转+拉伸模式进行特殊优化（最常见的情况）
    if (rotation == 90 && scale_mode == DisplayConfig::SCALE_STRETCH && 
        quality == DisplayConfig::QUALITY_FAST) {
        
        // 直接优化的90度旋转+拉伸
        for (uint32_t dst_y = 0; dst_y < dst_h; dst_y++) {
            uint32_t* dst_row = &dst_pixels[dst_y * dst_w];
            float norm_y = (float)dst_y / dst_h;
            uint32_t src_x_base = (uint32_t)(norm_y * src_w);
            
            for (uint32_t dst_x = 0; dst_x < dst_w; dst_x++) {
                float norm_x = (float)dst_x / dst_w;
                uint32_t src_y = (uint32_t)((1.0f - norm_x) * src_h);
                
                // 边界检查
                if (src_x_base < src_w && src_y < src_h) {
                    dst_row[dst_x] = src_pixels[src_y * src_w + src_x_base];
                } else {
                    dst_row[dst_x] = 0;
                }
            }
        }
        return;
    }
    
    // 通用变换路径
    for (uint32_t dst_y = 0; dst_y < dst_h; dst_y++) {
        for (uint32_t dst_x = 0; dst_x < dst_w; dst_x++) {
            uint32_t pixel = 0; // 默认黑色
            
            // 检查是否在有效区域内
            if (scale_mode == DisplayConfig::SCALE_STRETCH || 
                (dst_x >= offset_x && dst_x < offset_x + scaled_w &&
                 dst_y >= offset_y && dst_y < offset_y + scaled_h)) {
                
                // 计算在缩放后图像中的坐标
                float norm_x, norm_y;
                if (scale_mode == DisplayConfig::SCALE_STRETCH) {
                    norm_x = (float)dst_x / dst_w;
                    norm_y = (float)dst_y / dst_h;
                } else {
                    norm_x = (float)(dst_x - offset_x) / scaled_w;
                    norm_y = (float)(dst_y - offset_y) / scaled_h;
                }
                
                // 应用旋转变换
                float src_x_f, src_y_f;
                switch (rotation) {
                    case 90:
                        src_x_f = norm_y * src_w;
                        src_y_f = (1.0f - norm_x) * src_h;
                        break;
                    case 180:
                        src_x_f = (1.0f - norm_x) * src_w;
                        src_y_f = (1.0f - norm_y) * src_h;
                        break;
                    case 270:
                        src_x_f = (1.0f - norm_y) * src_w;
                        src_y_f = norm_x * src_h;
                        break;
                    default: // 0度
                        src_x_f = norm_x * src_w;
                        src_y_f = norm_y * src_h;
                        break;
                }
                
                // 获取像素值 - 优化版本
                if (quality == DisplayConfig::QUALITY_FAST) {
                    // 最近邻插值 - 优化边界检查
                    uint32_t src_x = (uint32_t)src_x_f;
                    uint32_t src_y = (uint32_t)src_y_f;
                    
                    // 使用位运算优化边界检查
                    if ((src_x | src_y) < ((src_w < src_h) ? src_w : src_h) && 
                        src_x < src_w && src_y < src_h) {
                        pixel = src_pixels[src_y * src_w + src_x];
                    }
                } else {
                    // 双线性插值
                    uint32_t src_x = (uint32_t)src_x_f;
                    uint32_t src_y = (uint32_t)src_y_f;
                    if (src_x < src_w - 1 && src_y < src_h - 1) {
                        float fx = src_x_f - src_x;
                        float fy = src_y_f - src_y;
                        
                        uint32_t p00 = src_pixels[src_y * src_w + src_x];
                        uint32_t p01 = src_pixels[src_y * src_w + src_x + 1];
                        uint32_t p10 = src_pixels[(src_y + 1) * src_w + src_x];
                        uint32_t p11 = src_pixels[(src_y + 1) * src_w + src_x + 1];
                        
                        uint32_t r = (uint32_t)(
                            ((p00 >> 16) & 0xFF) * (1-fx) * (1-fy) +
                            ((p01 >> 16) & 0xFF) * fx * (1-fy) +
                            ((p10 >> 16) & 0xFF) * (1-fx) * fy +
                            ((p11 >> 16) & 0xFF) * fx * fy) & 0xFF;
                        
                        uint32_t g = (uint32_t)(
                            ((p00 >> 8) & 0xFF) * (1-fx) * (1-fy) +
                            ((p01 >> 8) & 0xFF) * fx * (1-fy) +
                            ((p10 >> 8) & 0xFF) * (1-fx) * fy +
                            ((p11 >> 8) & 0xFF) * fx * fy) & 0xFF;
                        
                        uint32_t b = (uint32_t)(
                            (p00 & 0xFF) * (1-fx) * (1-fy) +
                            (p01 & 0xFF) * fx * (1-fy) +
                            (p10 & 0xFF) * (1-fx) * fy +
                            (p11 & 0xFF) * fx * fy) & 0xFF;
                        
                        pixel = 0xFF000000 | (r << 16) | (g << 8) | b;
                    } else if (src_x < src_w && src_y < src_h) {
                        pixel = src_pixels[src_y * src_w + src_x];
                    }
                }
            }
            
            dst_pixels[dst_y * dst_w + dst_x] = pixel;
        }
    }
}