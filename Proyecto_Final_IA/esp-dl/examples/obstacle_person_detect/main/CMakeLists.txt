set(src_dirs        ./)

set(include_dirs    ./)

set(requires        obstacle_person_detect spiffs esp_timer esp_cam_sensor esp_driver_cam esp_driver_isp esp_driver_i2c esp_sccb_intf esp_hw_support)

if (IDF_TARGET STREQUAL "esp32s3")
    list(APPEND requires esp32_s3_eye_noglib
                         esp_lcd)
elseif (IDF_TARGET STREQUAL "esp32p4")
    list(APPEND requires esp32_p4_function_ev_board_noglib
                         esp_lcd)
endif()

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

spiffs_create_partition_image(spiffs "images" FLASH_IN_PROJECT)
