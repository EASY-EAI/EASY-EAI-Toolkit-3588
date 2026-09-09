include(${CMAKE_CURRENT_LIST_DIR}/../../env.cmake)
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
file(GLOB QRDECODE_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# static Library paths
set(QRDECODE_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/${CMAKE_BOARDSYS}
    ${OpenCV_LIBS_DIRS}
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
    stdc++ 
    )
