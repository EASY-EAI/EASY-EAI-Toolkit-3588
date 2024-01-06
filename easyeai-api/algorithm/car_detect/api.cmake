find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB CARDETECT_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(CARDETECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(CARDETECT_LIBS 
    car_detect
    rknn_api 
    ${OpenCV_LIBS} 
    pthread
    )
