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

#若要使用${CMAKE_SYSROOT}变量，project必须要在set(CMAKE_TOOLCHAIN_FILE "${HOST_DIR}/share/buildroot/toolchainfile.cmake")后面设置
project(${CURRENT_FOLDER})
add_definitions(-Wall -Wpointer-arith)
# use c++ 11
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wall")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fPIC -Wall")

# ======================== easyeai_api =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/../../easyeai-api)
set(algorithm_root ${toolkit_root}/algorithm)
set(common_root    ${toolkit_root}/common)

include (${common_root}/system_opt/api.cmake)
include (${common_root}/gst_opt/api.cmake)
include (${common_root}/log_manager/api.cmake)
include (${common_root}/framequeue/api.cmake)
include (${common_root}/codec/api.cmake)
include (${common_root}/rtsp/api.cmake)
## api头文件路径
set(api_inc 
    ${SYSOPT_INCLUDE_DIRS} 
    ${GSTOPT_INCLUDE_DIRS} 
    ${LOGMGR_INCLUDE_DIRS} 
    ${FQUEUE_INCLUDE_DIRS} 
    ${CODEC_INCLUDE_DIRS} 
    ${RTSP_INCLUDE_DIRS} 
    )
## api源文件路径
file(GLOB api_srcs 
    ${SYSOPT_SOURCE_DIRS} 
    )
## api静态库文件路径
link_directories( 
    ${GSTOPT_LIBS_DIRS} 
    ${FQUEUE_LIBS_DIRS} 
    ${CODEC_LIBS_DIRS} 
    ${RTSP_LIBS_DIRS} 
    ${LOGMGR_LIBS_DIRS} 
)
## api所依赖的库参数
set(sysLib_list 
    ${SYSOPT_LIBS} 
    ${GSTOPT_LIBS} 
    ${FQUEUE_LIBS} 
    ${CODEC_LIBS} 
    ${RTSP_LIBS} 
    ${LOGMGR_LIBS} 
    )
# ======================== easyeai_api =========================

# ========================== custom ============================
# solution project
## 在/usr/share/cmake-3.18/Modules/目录下的Find*.cmake，都能通过这种方式被找到
find_package(OpenCV REQUIRED)

## 在/usr/lib/aarch64-linux-gnu/pkgconfig/下的pc文件，都可以通过这种方式查找
## 用法：
## find_package(PkgConfig)
## pkg_search_module(<prefix> REQUIRED [xxxx])
## pkg_search_module(<prefix1> REQUIRED [xxxx])
## pkg_search_module(<prefix2> REQUIRED [xxxx] [xxxxxxxxxxx])
## 备注：
##   <prefix>_FOUND：如果找到模块，此变量为1
##   <prefix>_LIBRARIES：模块的链接库名称（不包含-l）
##   <prefix>_LINK_LIBRARIES：模块的链接库名称（包含绝对路径）
##   <prefix>_LIBRARY_DIRS：模块的链接库路径（不包含-L）
##   <prefix>_INCLUDE_DIRS：模块的头文件路径（不包含-I）
##   <prefix>_LDFLAGS：链接flags
##   <prefix>_LDFLAGS_OTHER：额外的链接flags
##   <prefix>_CFLAGS：所有的编译器cflags
##   <prefix>_CFLAGS_OTHER：其他的编译器cflags
## pkg_check_modules 和 pkg_search_module 的异/同：
##   同：功能都是相同的，而且都能搜索多个模块，如：pkg_check_modules(AAAAA REQUIRED libxml-2.0 libxml2 libxml>=2)
##   异：
##       pkg_check_modules: 必须所有模块都存在，才能生效
##       pkg_search_module: 只要第一个模块存在，就能生效

find_package(PkgConfig)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
#pkg_search_module(GST1.0 REQUIRED gstreamer-1.0)
#pkg_check_modules(GST1.0_VIDEO REQUIRED gstreamer-video-1.0)

# -I
set(custom_inc
    ./src
    ${OpenCV_INCLUDE_DIRS} 
    ${GTK3_INCLUDE_DIRS} 
    #${GST1.0_INCLUDE_DIRS} 
    #${GST1.0_VIDEO_INCLUDE_DIRS} 
    ${PROJECT_SOURCE_DIR}/include
)
# -L
link_directories(
   #${PROJECT_SOURCE_DIR}/libs 
)
# -l
set(custom_libs
    rga 
    ${OpenCV_LIBS} 
    ${GTK3_LIBRARIES} 
    #${GST1.0_LIBRARIES} 
    #${GST1.0_VIDEO_LIBRARIES} 
)
# source code
aux_source_directory(./src dir_srcs)
aux_source_directory(./src/gstSource dir_srcs)
aux_source_directory(./src/gstSource/camera dir_srcs)
# ========================== custom ============================


#--------------------------
# execute file
#--------------------------
add_executable(${CURRENT_FOLDER} ${dir_srcs} ${api_srcs})   #-o
target_include_directories(${CURRENT_FOLDER} PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(${CURRENT_FOLDER} ${custom_libs} ${sysLib_list})

