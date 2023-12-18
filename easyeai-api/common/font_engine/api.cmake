
##在/usr/share/cmake-3.18/Modules/目录下的Find*.cmake，都能通过这种方式被找到
##具体的变量(如${OpenCV_INCLUDE_DIRS}、${OpenCV_LIBS})会被定义在里面的相对应的Find*.cmake文件中(通常就写在开头的描述里)
find_package(OpenCV REQUIRED)

##在/usr/lib/aarch64-linux-gnu/pkgconfig/下的pc文件，都可以通过这种方式查找
find_package(PkgConfig)
pkg_search_module(FREETYPE2 REQUIRED freetype2)

# source code path
file(GLOB FONTENGINE_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# headfile path
set(FONTENGINE_INCLUDE_DIRS
    ${OpenCV_INCLUDE_DIRS}
    ${FREETYPE2_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR} 
    ${CMAKE_CURRENT_LIST_DIR}/iconv 
    )

# c/c++ flags
set(FONTENGINE_LIBS 
    ${OpenCV_LIBS}
    ${FREETYPE2_LIBRARY_DIRS}
    freetype
    stdc++ 
    )
