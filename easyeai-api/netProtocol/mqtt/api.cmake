# source code path
file(GLOB MQTT_SOURCE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/*.c 
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp 
    )

# static Library paths
file(GLOB MQTT_LIBS_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
    )
    
# headfile path
set(MQTT_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR} 
    )

# c/c++ flags
set(MQTT_LIBS 
    mosquitto 
    crypto 
    ssl 
    pthread 
    stdc++ 
    )
