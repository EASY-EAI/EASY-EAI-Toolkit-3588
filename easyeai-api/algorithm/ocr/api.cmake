find_package(OpenCV REQUIRED)

# static Library paths
file(GLOB OCR_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(OCR_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    ${OpenCV_INCLUDE_DIRS} 
    )

# c/c++ flags
set(OCR_LIBS 
    ocr
    rknn_api
    ${OpenCV_LIBS} 
    pthread
    )
