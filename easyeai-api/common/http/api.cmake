
##在/usr/share/cmake-3.18/Modules/目录下的Find*.cmake，都能通过这种方式被找到
##具体的变量(如${CURL_INCLUDE_DIRS}、${CURL_LIBRARIES})会被定义在里面的相对应的Find*.cmake文件中(通常就写在开头的描述里)
find_package(CURL REQUIRED)

# source code path
file(GLOB HTTP_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# headfile path
set(HTTP_INCLUDE_DIRS
    ${CURL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR} 
    )

# c/c++ flags
set(HTTP_LIBS 
    ${CURL_LIBRARIES}
    stdc++ 
    )
