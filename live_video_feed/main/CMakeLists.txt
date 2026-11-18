idf_component_register(SRCS "main.cpp" "mipi_camera.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-dl obstacle_person_detect esp_timer)
