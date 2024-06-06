find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB FACE_DETECT_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(FACE_DETECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(FACE_DETECT_LIBS 
    face_detect
    rknn_api 
    ${OpenCV_LIBS} 
    pthread
    )
