find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB FACE_RECOGNITION_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(FACE_RECOGNITION_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(FACE_RECOGNITION_LIBS 
    face_recognition
    rknn_api
    ${OpenCV_LIBS} 
    pthread
    )
