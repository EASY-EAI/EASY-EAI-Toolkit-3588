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

include (${common_root}/base64/api.cmake)
include (${common_root}/system_opt/api.cmake)
## api头文件路径
set(api_inc 
    ${BASE64_INCLUDE_DIRS} 
    ${SYSOPT_INCLUDE_DIRS} 
    )
## api源文件路径
file(GLOB api_srcs 
    ${BASE64_SOURCE_DIRS} 
    ${SYSOPT_SOURCE_DIRS} 
    )
## api静态库文件路径
#set(staticLib_objs 
#    )
## 依赖的动态库文件
set(sysLib_list 
    ${BASE64_LIBS} 
    ${SYSOPT_LIBS} 
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
find_package(PkgConfig)
pkg_search_module(GST1.0 REQUIRED gstreamer-1.0)

# -I
set(custom_inc
    #${CMAKE_SYSROOT}/usr/include/rkfacial 
    ${OpenCV_INCLUDE_DIRS} 
    ${GST1.0_INCLUDE_DIRS} 
    ${PROJECT_SOURCE_DIR}/include
)
# -L
link_directories(
   #${PROJECT_SOURCE_DIR}/libs 
)
# -l
set(custom_libs
    ${OpenCV_LIBS}
    ${GST1.0_LIBRARIES}
)
# source code
aux_source_directory(./src dir_srcs)
aux_source_directory(./src/playChannel dir_srcs)
# ========================== custom ============================


#--------------------------
# execute file
#--------------------------
add_executable(${CURRENT_FOLDER} ${dir_srcs} ${api_srcs})		#-o
target_include_directories(${CURRENT_FOLDER} PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(${CURRENT_FOLDER} ${custom_libs} ${sysLib_list})
