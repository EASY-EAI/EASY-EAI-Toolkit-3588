
## get board rootfs version
set(BOARDSYS_ID "")
set(BOARDSYS_VERSION_ID "")
if(EXISTS "${CMAKE_SYSROOT}/etc/os-release")
    # 按行读取文件
    file(STRINGS "${CMAKE_SYSROOT}/etc/os-release" OS_RELEASE_LINES)

    foreach(line ${OS_RELEASE_LINES})
        # 匹配 ID=xxx 或 ID="xxx"
        if(line MATCHES "^ID=\"?([^\"\n]+)\"?")
            set(BOARDSYS_ID "${CMAKE_MATCH_1}")
        endif()

        # 匹配 VERSION_ID=xxx
        if(line MATCHES "^VERSION_ID=\"?([^\"\n]+)\"?")
            set(BOARDSYS_VERSION_ID "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()
set(CMAKE_BOARDSYS ${BOARDSYS_ID}${BOARDSYS_VERSION_ID})