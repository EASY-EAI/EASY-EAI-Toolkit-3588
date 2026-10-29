cmake_minimum_required(VERSION 3.10.2)

STRING(REGEX REPLACE ".*/(.*)" "\\1" CURRENT_FOLDER ${CMAKE_CURRENT_SOURCE_DIR})
MESSAGE("current project: " ${CURRENT_FOLDER})

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_CROSSCOMPILING TRUE)

cmake_host_system_information(RESULT arch_value QUERY OS_PLATFORM)
message(STATUS "architecture: " ${arch_value})

if((NOT "${arch_value}" STREQUAL "armv7l") AND (NOT "${arch_value}" STREQUAL "aarch64"))
    include ($ENV{HOME}/configs/cross.cmake)
endif()

project(${CURRENT_FOLDER})
add_definitions(-Wall -Wpointer-arith)

# ======================== easyeai_api =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/../../easyeai-api)
set(common_root ${toolkit_root}/common)
set(media_root ${toolkit_root}/media)

include (${common_root}/system_opt/api.cmake)
# display 模块(display_pro) 与工程内显示通路监控依赖 log_manager_pro
# 的日志接口 (log_manager_init / log_register / log_print)。
include (${common_root}/log_manager_pro/api.cmake)
set(api_inc
    ${SYSOPT_INCLUDE_DIRS}
    ${media_root}/display
    ${media_root}/gpu
    ${media_root}/rga
    ${media_root}/camera
    ${LOGMANAGERPRO_INCLUDE_DIRS}
    )
file(GLOB api_srcs
    ${SYSOPT_SOURCE_DIRS}
    ${media_root}/gpu/gpu_util.c
    ${media_root}/rga/rga_util.c
    ${media_root}/camera/camera_discover.c
    ${media_root}/camera/capture.c
    ${media_root}/camera/mipi_camera.c
    )
# 链接顺序: display 在前, log_manager_pro 在后(静态库解析依赖)。
set(sysLib_list
    ${SYSOPT_LIBS}
    display
    log_manager_pro
    )
link_directories(
    ${media_root}/display/${CMAKE_BOARDSYS}
    ${LOGMANAGERPRO_LIBS_DIRS}
    )
# ======================== easyeai_api =========================

set(custom_inc
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_SYSROOT}/usr/include/libdrm
    ${CMAKE_SYSROOT}/usr/include/rga
)
set(custom_libs
    pthread
    drm
    rga
    # 只链 libmali（自带 EGL/GLES/GBM）。不要再链 Mesa 的 -lEGL/-lGLESv2/-lgbm，
    # 否则会与 libmali 内置 GBM 混库导致崩溃。
    # 依赖交叉 sysroot 中的 libmali.so 符号链接，编译前部署方法见 README。
    mali
)
aux_source_directory(./src dir_srcs)

add_executable(${CURRENT_FOLDER} ${dir_srcs} ${api_srcs})
target_include_directories(${CURRENT_FOLDER} PRIVATE ${custom_inc} ${api_inc})
target_compile_options(${CURRENT_FOLDER} PRIVATE -Wextra -Wno-unused-parameter)
target_link_libraries(${CURRENT_FOLDER} ${custom_libs} ${sysLib_list})
# 强制从 libmali 解析 EGL/GLES 符号，避免偶发拉到 Mesa。
target_link_options(${CURRENT_FOLDER} PRIVATE
    -Wl,--no-as-needed
    -Wl,-Bsymbolic
)
