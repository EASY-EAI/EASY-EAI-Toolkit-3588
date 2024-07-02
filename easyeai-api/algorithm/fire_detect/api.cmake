find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB FIREDETECT_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(FIREDETECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(FIREDETECT_LIBS 
    fire_detect
    rknn_api 
    ${OpenCV_LIBS} 
    pthread
    )
