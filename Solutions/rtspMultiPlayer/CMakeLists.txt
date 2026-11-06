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
set(media_root     ${toolkit_root}/media)

include (${common_root}/base64/api.cmake)
include (${common_root}/system_opt/api.cmake)
include (${common_root}/log_manager_pro/api.cmake)
include (${media_root}/gst_opt/api.cmake)
## api头文件路径
set(api_inc 
    ${BASE64_INCLUDE_DIRS} 
    ${SYSOPT_INCLUDE_DIRS} 
    ${LOGMANAGERPRO_INCLUDE_DIRS}
    ${GSTOPT_INCLUDE_DIRS} 
    ${media_root}/display
    )
## api源文件路径
file(GLOB api_srcs 
    ${BASE64_SOURCE_DIRS} 
    ${SYSOPT_SOURCE_DIRS} 
    )
## api静态库文件路径
link_directories(
    ${GSTOPT_LIBS_DIRS} 
    ${media_root}/display/${CMAKE_BOARDSYS}
    ${LOGMANAGERPRO_LIBS_DIRS}
)
## api所依赖的库参数
set(sysLib_list 
    ${BASE64_LIBS} 
    ${SYSOPT_LIBS} 
    ${GSTOPT_LIBS} 
    drm
    display
    log_manager_pro
    )
# ======================== easyeai_api =========================

# ========================== custom ============================
# solution project
## 在/usr/share/cmake-3.18/Modules/目录下的Find*.cmake，都能通过这种方式被找到
#find_package(OpenCV REQUIRED)
set(OpenCV_INCLUDE_DIRS
    ${CMAKE_SYSROOT}/usr/include/ 
    ${CMAKE_SYSROOT}/usr/include/opencv4/ 
)
set(OpenCV_LIBS_DIRS
    ${CAMKE_SYSROOT}/usr/lib/aarch64-linux-gnu/lapack
    ${CAMKE_SYSROOT}/usr/lib/aarch64-linux-gnu/blas
)

## 显示走 DRM/KMS（easyeai-api/media/display），不再依赖 GTK

# -I
set(custom_inc
    ${OpenCV_INCLUDE_DIRS} 
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
)
# source code
aux_source_directory(./src dir_srcs)
aux_source_directory(./src/capturer dir_srcs)
aux_source_directory(./src/analyzer dir_srcs)
aux_source_directory(./src/player dir_srcs)
# ========================== custom ============================


#--------------------------
# execute file
#--------------------------
add_executable(${CURRENT_FOLDER} ${dir_srcs} ${api_srcs})   #-o
target_include_directories(${CURRENT_FOLDER} PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(${CURRENT_FOLDER} ${custom_libs} ${sysLib_list})

