#pragma once

#define CAM_WIDTH  800
#define CAM_HEIGHT 800

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mipi_camera_init(void);
esp_err_t mipi_camera_capture(void **frame_buffer, size_t *size);

#ifdef __cplusplus
}
#endif
