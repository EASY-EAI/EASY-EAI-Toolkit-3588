# source code path
file(GLOB CJSON_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# headfile path
set(CJSON_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    )

# c/c++ flags
set(CJSON_LIBS 
    pthread
    stdc++ 
    )
