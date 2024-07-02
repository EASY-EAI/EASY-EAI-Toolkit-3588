find_package(OpenCV REQUIRED)

# source code path
file(GLOB QRDECODE_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# static Library paths
file(GLOB QRDECODE_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(QRDECODE_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(QRDECODE_LIBS
    zbar 
    ${OpenCV_LIBS} 
    pthread
    )
