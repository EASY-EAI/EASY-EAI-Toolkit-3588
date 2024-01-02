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
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wall ")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fPIC -Wall -pthread")

# ======================== easyeai_api =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/../../easyeai-api)
set(algorithm_root ${toolkit_root}/algorithm)
set(common_root    ${toolkit_root}/common)

include (${common_root}/http/api.cmake)
## api头文件路径
set(api_inc 
#    ${CMAKE_SYSROOT}/usr/include/rkfacial 
    ${HTTP_INCLUDE_DIRS} 
    )
## api源文件路径
file(GLOB api_srcs 
    ${HTTP_SOURCE_DIRS} 
#    ./*.c ./*.cpp 
    )
## api静态库文件路径
#set(staticLib_objs 
#    )
## 依赖的动态库文件
set(sysLib_list 
    ${HTTP_LIBS} 
    )

#--------------------------
# test-http_client
#--------------------------
set(demoTag test-http_client)
add_executable(${demoTag} ${demoTag}.c ${api_srcs}) #-o
target_link_libraries(${demoTag} ${sysLib_list}) #-l
target_include_directories(${demoTag} PRIVATE ${api_inc}) #-I

#--------------------------
# test-http_server
#--------------------------
set(demoTag test-http_server)
add_executable(${demoTag} ${demoTag}.cpp ${api_srcs}) #-o
target_link_libraries(${demoTag} ${sysLib_list}) #-l
target_include_directories(${demoTag} PRIVATE ${api_inc}) #-I

