
find_package(OpenCV REQUIRED)

# source code path
file(GLOB QRCODE_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    ${CMAKE_CURRENT_LIST_DIR}/qrencode/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/qrencode/*.cpp 
    )

# headfile path
set(QRCODE_INCLUDE_DIRS 
    ${OpenCV_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR} 
    ${CMAKE_CURRENT_LIST_DIR}/qrencode 
    )

# c/c++ flags
set(QRCODE_LIBS 
    ${OpenCV_LIBS}
    pthread 
    stdc++ 
    )
