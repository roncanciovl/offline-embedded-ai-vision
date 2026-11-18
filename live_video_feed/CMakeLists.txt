cmake_minimum_required(VERSION 3.16)

# Add ESP-DL components using relative paths
set(EXTRA_COMPONENT_DIRS 
    "${CMAKE_CURRENT_SOURCE_DIR}/../Proyecto_Final_IA/esp-dl/esp-dl"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Proyecto_Final_IA/esp-dl/models/obstacle_person_detect"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-detection-live-feed)

