# source code path
file(GLOB RKMPI_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# headfile path
set(RKMPI_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    #这里注释掉，是为了不用/usr/include/rockchip/里面的rk_type.h
    #/usr/include/rockchip 
    )

# c/c++ flags
set(RKMPI_LIBS 
    rockit
    )
