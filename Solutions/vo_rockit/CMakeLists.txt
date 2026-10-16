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

# ======================== easyeai_api =========================
set(toolkit_root ${CMAKE_CURRENT_SOURCE_DIR}/../../easyeai-api)
set(common_root    ${toolkit_root}/common)

include (${common_root}/system_opt/api.cmake)
include (${common_root}/rk_mpi/api.cmake)
## api头文件路径
set(api_inc
    ${SYSOPT_INCLUDE_DIRS}
    ${RKMPI_INCLUDE_DIRS}
    )
## api源文件路径
file(GLOB api_srcs
    ${SYSOPT_SOURCE_DIRS}
    ${RKMPI_SOURCE_DIRS}
    )
## api所依赖的库参数
set(sysLib_list
    ${SYSOPT_LIBS}
    ${RKMPI_LIBS}
    )
# ======================== easyeai_api =========================

# ========================== custom ============================
# -I
set(custom_inc
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
# -l
set(custom_libs
    pthread
)
# source code
aux_source_directory(./src dir_srcs)
aux_source_directory(./src/capturer dir_srcs)
aux_source_directory(./src/player dir_srcs)
aux_source_directory(./src/ui dir_srcs)
# ========================== custom ============================


#--------------------------
# execute file
#--------------------------
add_executable(${CURRENT_FOLDER} ${dir_srcs} ${api_srcs})   #-o
target_include_directories(${CURRENT_FOLDER} PRIVATE ${custom_inc} ${api_inc})
target_link_libraries(${CURRENT_FOLDER} ${custom_libs} ${sysLib_list})
