#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mipi_camera.h"
#include "espdet_detect.hpp"
#include "mbedtls/base64.h"
#include "dl_image_jpeg.hpp"

static const char *TAG = "live_feed";

// 7 FPS constraint -> roughly 142ms per frame
#define TARGET_FRAME_TIME_US (1000000 / 7)

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-P4 Live Feed Object Detection");

    // 1. Initialize MIPI Camera
    ESP_ERROR_CHECK(mipi_camera_init());

    // 2. Initialize Model
    ESP_LOGI(TAG, "Initializing ESP-DL Model");
    ESPDetDetect *detect = new ESPDetDetect(ESPDetDetect::ESPDET_PICO_224_224_OBSTACLE_PERSON);

    while (true) {
        int64_t start_time = esp_timer_get_time();

        // 3. Capture Frame
        void *frame_buffer = NULL;
        size_t size = 0;
        if (mipi_camera_capture(&frame_buffer, &size) == ESP_OK) {
            
            // Software debayer RAW8 -> RGB888
            size_t rgb_size = CAM_WIDTH * CAM_HEIGHT * 3;
            uint8_t* rgb_buffer = (uint8_t*)heap_caps_aligned_alloc(16, rgb_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
            if (rgb_buffer) {
                uint8_t* raw8_buffer = (uint8_t*)frame_buffer;
                
                // --- DEBUG ADDITION ---
                long long pixel_sum = 0;
                for (int i = 0; i < CAM_WIDTH * CAM_HEIGHT; i++) {
                    pixel_sum += raw8_buffer[i];
                }
                ESP_LOGI(TAG, "Raw frame average pixel value: %d", (int)(pixel_sum / (CAM_WIDTH * CAM_HEIGHT)));
                if (pixel_sum == 0) {
                    ESP_LOGW(TAG, "Camera is outputting a COMPLETELY BLACK image (all zeros)!");
                }
                // Convert RAW8 Bayer (BGGR) to RGB888
                uint8_t *raw = (uint8_t *)frame_buffer;
                for (int y = 0; y < 800 - 1; y += 2) {
                    for (int x = 0; x < 800 - 1; x += 2) {
                        int i00 = y * 800 + x;
                        int i01 = i00 + 1;
                        int i10 = (y + 1) * 800 + x;
                        int i11 = i10 + 1;

                        uint8_t b  = raw[i00];
                        uint8_t g1 = raw[i01];
                        uint8_t g2 = raw[i10];
                        uint8_t r  = raw[i11];

                        uint8_t g = (g1 + g2) / 2;

                        // Pixel (x, y)
                        int out_i = (y * 800 + x) * 3;
                        rgb_buffer[out_i] = r; rgb_buffer[out_i+1] = g; rgb_buffer[out_i+2] = b;

                        // Pixel (x+1, y)
                        out_i += 3;
                        rgb_buffer[out_i] = r; rgb_buffer[out_i+1] = g; rgb_buffer[out_i+2] = b;

                        // Pixel (x, y+1)
                        out_i = ((y + 1) * 800 + x) * 3;
                        rgb_buffer[out_i] = r; rgb_buffer[out_i+1] = g; rgb_buffer[out_i+2] = b;

                        // Pixel (x+1, y+1)
                        out_i += 3;
                        rgb_buffer[out_i] = r; rgb_buffer[out_i+1] = g; rgb_buffer[out_i+2] = b;
                    }
                }
                // Removed old Bayer logic

                // 4. Run Inference
                // Our model expects an input tensor. The image preprocessor handles it.
                dl::image::img_t img = {rgb_buffer, CAM_WIDTH, CAM_HEIGHT, dl::image::DL_IMAGE_PIX_TYPE_RGB888};
                auto &detect_res = detect->run(img);

                // Compress to JPEG using ESP32-P4 hardware encoder
                dl::image::jpeg_img_t jpeg = dl::image::hw_encode_jpeg(img, 20);
                
                if (jpeg.data == NULL || jpeg.data_len == 0) {
                    ESP_LOGE(TAG, "Hardware JPEG encoding failed!");
                } else {
                    ESP_LOGI(TAG, "JPEG encoded size: %d bytes", jpeg.data_len);
                }

                // Base64 encode the JPEG
                size_t b64_len = 0;
                mbedtls_base64_encode(NULL, 0, &b64_len, (const unsigned char*)jpeg.data, jpeg.data_len);
                unsigned char *b64_buf = (unsigned char *)malloc(b64_len);
                if (b64_buf) {
                    mbedtls_base64_encode(b64_buf, b64_len, &b64_len, (const unsigned char*)jpeg.data, jpeg.data_len);
                    
                    // 5. Build and Send JSON over Serial
                    printf("\n>>>JSON_START<<<\n");
                    printf("{\n");
                    printf("  \"image\": \"%s\",\n", b64_buf);
                    printf("  \"detections\": [\n");
                    int i = 0;
                    int total = detect_res.size();
                    for (const auto &res : detect_res) {
                        printf("    {\"category\": %d, \"score\": %f, \"box\": [%d, %d, %d, %d]}%s\n",
                               res.category, res.score, res.box[0], res.box[1], res.box[2], res.box[3],
                               (i == total - 1) ? "" : ",");
                        i++;
                    }
                    printf("  ]\n");
                    printf("}\n");
                    printf(">>>JSON_END<<<\n\n");
                    
                    free(b64_buf);
                }
                
                free(jpeg.data);
                free(rgb_buffer);
            }

            // Yield to idle task to reset the watchdog timer
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            ESP_LOGE(TAG, "Camera capture failed");
        }

        // 6. Frame Rate Limiter (7 fps)
        int64_t end_time = esp_timer_get_time();
        int64_t elapsed_us = end_time - start_time;
        if (elapsed_us < TARGET_FRAME_TIME_US) {
            vTaskDelay(pdMS_TO_TICKS((TARGET_FRAME_TIME_US - elapsed_us) / 1000));
        }
    }
}
