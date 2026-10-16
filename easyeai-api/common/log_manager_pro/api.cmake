include(${CMAKE_CURRENT_LIST_DIR}/../../env.cmake)
# source code path
file(GLOB LOGMANAGERPRO_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
    )

# static Library paths
file(GLOB LOGMANAGERPRO_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/${CMAKE_BOARDSYS}
    )

# headfile path
set(LOGMANAGERPRO_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )

# c/c++ flags
set(LOGMANAGERPRO_LIBS
    log_manager_pro
    pthread
    )
