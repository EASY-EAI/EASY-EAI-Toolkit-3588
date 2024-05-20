find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB PERSON_DETECT_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(PERSON_DETECT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(PERSON_DETECT_LIBS 
    person_detect
    rknn_api 
    ${OpenCV_LIBS} 
    pthread
    )
