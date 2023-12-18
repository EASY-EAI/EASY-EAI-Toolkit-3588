# source code path
file(GLOB BASE64_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# headfile path
set(BASE64_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    )

# c/c++ flags
set(BASE64_LIBS 
    stdc++ 
    )
