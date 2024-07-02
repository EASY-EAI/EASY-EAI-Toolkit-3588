find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB HELMET_DETECT_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(HELMET_DETECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(HELMET_DETECT_LIBS 
    helmet_detect
    rknn_api 
    ${OpenCV_LIBS} 
    pthread
    )
