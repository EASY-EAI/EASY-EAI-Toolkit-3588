##在/usr/share/cmake-3.18/Modules/目录下的Find*.cmake，都能通过这种方式被找到
##具体的变量(如${OpenCV_INCLUDE_DIRS}、${OpenCV_LIBS})会被定义在里面的相对应的Find*.cmake文件中(通常就写在开头的描述里)
#find_package(OpenCV REQUIRED)
#
set(OpenCV_INCLUDE_DIRS
    ${CMAKE_SYSROOT}/usr/include/ 
    ${CMAKE_SYSROOT}/usr/include/opencv4/ 
)
set(OpenCV_LIBS_DIRS
    ${CAMKE_SYSROOT}/usr/lib/aarch64-linux-gnu/lapack
    ${CAMKE_SYSROOT}/usr/lib/aarch64-linux-gnu/blas
)
set(OpenCV_LIBS
    opencv_core 
    opencv_imgproc 
    opencv_imgcodecs
#    opencv_calib3d 
#    opencv_dnn 
#    opencv_features2d 
#    opencv_flann 
#    opencv_highgui 
#    opencv_ml 
#    opencv_objdetect 
#    opencv_photo 
#    opencv_stitching
#    opencv_videoio 
#    opencv_video  
)

# source code path
file(GLOB QRCODE_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    ${CMAKE_CURRENT_LIST_DIR}/qrencode/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/qrencode/*.cpp 
    )

# static Library paths
set(QRCODE_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    ${OpenCV_LIBS_DIRS}
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
