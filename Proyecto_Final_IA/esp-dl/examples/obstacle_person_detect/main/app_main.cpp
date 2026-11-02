#include "espdet_detect.hpp"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "dl_image_jpeg.hpp"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "obstacle_person_detect_spiffs";

static esp_err_t mount_spiffs()
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %u, used: %u", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

// True only if the file name ends with .jpg / .jpeg (case-insensitive).
static bool is_jpeg_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
}

extern "C" void app_main(void)
{
    if (mount_spiffs() != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "Loading ESP-DL model...");
    int64_t t0 = esp_timer_get_time();
    // lazy_load = false: load the model now instead of on the first run().
    ESPDetDetect *detect = new ESPDetDetect(ESPDetDetect::ESPDET_PICO_224_224_OBSTACLE_PERSON, false);
    ESP_LOGI(TAG, "Model loaded in %lld ms", (esp_timer_get_time() - t0) / 1000);

    while (true) {
        DIR *dir = opendir("/spiffs");
        if (dir == NULL) {
            ESP_LOGE(TAG, "Failed to open directory");
            break;
        }

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (!is_jpeg_file(ent->d_name)) {
                continue;
            }

            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/spiffs/%s", ent->d_name);
            ESP_LOGI(TAG, "Processing %s", filepath);

            // Read JPEG from SPIFFS
            dl::image::jpeg_img_t jpeg = dl::image::read_jpeg(filepath);
            if (jpeg.data == nullptr) {
                ESP_LOGE(TAG, "Failed to read %s", filepath);
                continue;
            }

            // Decode JPEG to RGB888
            int64_t t_dec = esp_timer_get_time();
            dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
            heap_caps_free(jpeg.data);
            if (img.data == nullptr) {
                ESP_LOGE(TAG, "Failed to decode %s", filepath);
                continue;
            }

            // Run inference (preprocess + model + postprocess)
            int64_t t_inf = esp_timer_get_time();
            auto &results = detect->run(img);
            int64_t t_end = esp_timer_get_time();

            int decode_ms = (int)((t_inf - t_dec) / 1000);
            int inference_ms = (int)((t_end - t_inf) / 1000);

            // Print results in JSON format for the Python script
            printf("\n>>>JSON_START<<<\n");
            printf("{\n");
            printf("  \"image\": \"%s\",\n", ent->d_name);
            printf("  \"width\": %d, \"height\": %d,\n", img.width, img.height);
            printf("  \"decode_ms\": %d, \"inference_ms\": %d,\n", decode_ms, inference_ms);
            printf("  \"detections\": [\n");

            size_t idx = 0;
            for (auto it = results.begin(); it != results.end(); ++it, ++idx) {
                const auto &res = *it;
                printf("    {\"category\": %d, \"score\": %f, \"box\": [%d, %d, %d, %d]}%s\n",
                       res.category,
                       res.score,
                       res.box[0],
                       res.box[1],
                       res.box[2],
                       res.box[3],
                       (idx == results.size() - 1) ? "" : ",");
            }
            printf("  ]\n");
            printf("}\n");
            printf(">>>JSON_END<<<\n\n");

            heap_caps_free(img.data);

            // Short delay to allow the Python script to process
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        closedir(dir);

        ESP_LOGI(TAG, "Finished processing all images. Waiting 5 seconds before restarting...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    delete detect;
    esp_vfs_spiffs_unregister("spiffs");
}
