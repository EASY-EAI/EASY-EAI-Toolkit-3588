include(${CMAKE_CURRENT_LIST_DIR}/../../env.cmake)
find_package(PkgConfig)
pkg_search_module(GST1.0 REQUIRED gstreamer-1.0)
pkg_search_module(GST1.0_VIDEO REQUIRED gstreamer-video-1.0)
pkg_search_module(GST1.0_SERVER REQUIRED gstreamer-rtsp-server-1.0)

# source code path
file(GLOB GSTOPT_SOURCE_DIRS 
    ${CMAKE_CURRENT_LIST_DIR}/src/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp 
    )

# static Library paths
file(GLOB GSTOPT_LIBS_DIRS 
    ${CMAKE_CURRENT_LIST_DIR}/${CMAKE_BOARDSYS}
    )
    
# headfile path
set(GSTOPT_INCLUDE_DIRS 
    ${CMAKE_CURRENT_LIST_DIR} 
    ${CMAKE_CURRENT_LIST_DIR}/include 
    ${CMAKE_CURRENT_LIST_DIR}/include_internal 
    ${GST1.0_INCLUDE_DIRS} 
    ${GST1.0_VIDEO_INCLUDE_DIRS} 
    ${GST1.0_SERVER_INCLUDE_DIRS} 
    )

# c/c++ flags
set(GSTOPT_LIBS 
    gst_opt
    ${GST1.0_LIBRARIES} 
    ${GST1.0_VIDEO_LIBRARIES} 
    ${GST1.0_SERVER_LIBRARIES} 
    )
