cmake_minimum_required(VERSION 3.16)
project(rk3588_multi_display)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find DRM
pkg_check_modules(DRM REQUIRED libdrm)

# Find GBM
pkg_check_modules(GBM REQUIRED gbm)

# Find EGL
find_package(OpenGL REQUIRED)
pkg_check_modules(EGL REQUIRED egl)

# RGA library (假设已安装)
find_library(RGA_LIB rga HINTS /usr/lib /usr/local/lib)
if(NOT RGA_LIB)
    message(WARNING "RGA library not found, using stub implementation")
endif()

# Find libinput for hotplug detection
pkg_check_modules(LIBINPUT libinput)
pkg_check_modules(LIBUDEV libudev)

# Find spdlog
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    pkg_check_modules(SPDLOG spdlog)
    if(NOT SPDLOG_FOUND)
        message(STATUS "spdlog not found in system, will try to use header-only version")
        find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h PATHS /usr/include /usr/local/include)
        if(SPDLOG_INCLUDE_DIR)
            set(SPDLOG_FOUND TRUE)
            set(SPDLOG_INCLUDE_DIRS ${SPDLOG_INCLUDE_DIR})
        else()
            message(WARNING "spdlog not found, falling back to iostream")
        endif()
    endif()
endif()

include_directories(
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    ${LIBUDEV_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS}
    src
)

set(SOURCES
    src/main.cpp
    src/drm_manager.cpp
    src/display_manager.cpp
    src/hotplug_detector.cpp
    src/rga_helper.cpp
    src/frame_copier.cpp
    src/logger.cpp
    src/system_checker.cpp
)

set(HEADERS
    src/drm_manager.h
    src/display_manager.h
    src/hotplug_detector.h
    src/rga_helper.h
    src/frame_copier.h
    src/logger.h
    src/system_checker.h
)

add_executable(rk3588_multi_display ${SOURCES} ${HEADERS})

target_link_libraries(rk3588_multi_display
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${LIBUDEV_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${EGL_LIBRARIES}
    pthread
)

if(RGA_LIB)
    target_link_libraries(rk3588_multi_display ${RGA_LIB})
    target_compile_definitions(rk3588_multi_display PRIVATE HAVE_RGA)
endif()

# Add spdlog support
if(spdlog_FOUND)
    target_link_libraries(rk3588_multi_display spdlog::spdlog)
    target_compile_definitions(rk3588_multi_display PRIVATE HAVE_SPDLOG)
elseif(SPDLOG_FOUND)
    target_compile_definitions(rk3588_multi_display PRIVATE HAVE_SPDLOG)
endif()

target_compile_options(rk3588_multi_display PRIVATE
    ${DRM_CFLAGS_OTHER}
    ${GBM_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${LIBINPUT_CFLAGS_OTHER}
    ${LIBUDEV_CFLAGS_OTHER}
)

# Install target
install(TARGETS rk3588_multi_display DESTINATION bin)

# Create systemd service
configure_file(
    ${CMAKE_SOURCE_DIR}/rk3588-multi-display.service.in
    ${CMAKE_BINARY_DIR}/rk3588-multi-display.service
    @ONLY
)

install(FILES ${CMAKE_BINARY_DIR}/rk3588-multi-display.service
        DESTINATION /etc/systemd/system/) 