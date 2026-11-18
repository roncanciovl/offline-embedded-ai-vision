
#include "espdet_detect.hpp"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "dl_image_jpeg.hpp"
#include "driver/i2c_master.h"
#include "driver/isp.h"
extern "C" {
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "ov5647.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_ldo_regulator.h"
#include "esp_cache.h"
#include "hal/uart_ll.h"
}
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "obstacle_person_detect";

// Camera MIPI-CSI and I2C Configuration for JC-ESP32P4-M3 with Raspberry Pi Camera (OV5647)
#define CAM_SCCB_SDA_IO         (GPIO_NUM_7)
#define CAM_SCCB_SCL_IO         (GPIO_NUM_8)
#define CAM_MIPI_CSI_HRES       (800)
#define CAM_MIPI_CSI_VRES       (640)
#define CAM_LANE_BITRATE_MBPS   (200) // 200 Mbps for OV5647 800x640 50fps RAW8
#define CAM_BUFFER_SIZE         (CAM_MIPI_CSI_HRES * CAM_MIPI_CSI_VRES * 2) // RGB565 = 2 bytes per pixel

#define THUMB_W                 (266)
#define THUMB_H                 (212)
#define THUMB_RGB888_SIZE       (THUMB_W * THUMB_H * 3)

static esp_cam_ctlr_handle_t s_cam_handle = NULL;
static esp_sccb_io_handle_t s_sccb_handle = NULL;
static uint8_t *s_cam_buffer = NULL;
static size_t s_cam_aligned_size = 0;
static uint8_t *s_thumb_rgb888 = NULL;
static char *s_b64_buffer = NULL;
static esp_cam_ctlr_trans_t s_active_trans = {};
static SemaphoreHandle_t s_frame_sem = NULL;

static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *src, size_t len, char *dst)
{
    size_t i = 0, j = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[j++] = B64_TABLE[(v >> 18) & 0x3F];
        dst[j++] = B64_TABLE[(v >> 12) & 0x3F];
        dst[j++] = B64_TABLE[(v >> 6) & 0x3F];
        dst[j++] = B64_TABLE[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)src[i + 1] << 8;
        }
        dst[j++] = B64_TABLE[(v >> 18) & 0x3F];
        dst[j++] = B64_TABLE[(v >> 12) & 0x3F];
        dst[j++] = (i + 1 < len) ? B64_TABLE[(v >> 6) & 0x3F] : '=';
        dst[j++] = '=';
    }
    dst[j] = '\0';
    return j;
}

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    if (user_data) {
        esp_cam_ctlr_trans_t *p_trans = (esp_cam_ctlr_trans_t *)user_data;
        trans->buffer = p_trans->buffer;
        trans->buflen = p_trans->buflen;
        trans->received_size = p_trans->buflen;
    }
    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    BaseType_t high_task_woken = pdFALSE;
    if (s_frame_sem) {
        xSemaphoreGiveFromISR(s_frame_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t mount_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = "spiffs";
    conf.max_files = 5;
    conf.format_if_mount_failed = false;

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS partition size: total: %d, used: %d", (int)total, (int)used);
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

static bool init_camera(void)
{
    ESP_LOGI(TAG, "Configuring MIPI-CSI Camera subsystem (OV5647)...");

    // 1. Power MIPI CSI PHY via LDO VO3 (2.5V)
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {};
    ldo_mipi_phy_config.chan_id = 3;
    ldo_mipi_phy_config.voltage_mv = 2500;

    esp_err_t ret = esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LDO acquire note: %s", esp_err_to_name(ret));
    }

    // 2. Initialize I2C Master Bus on GPIO 7 (SDA) and GPIO 8 (SCL)
    i2c_master_bus_config_t i2c_bus_conf = {};
    i2c_bus_conf.i2c_port = I2C_NUM_0;
    i2c_bus_conf.sda_io_num = CAM_SCCB_SDA_IO;
    i2c_bus_conf.scl_io_num = CAM_SCCB_SCL_IO;
    i2c_bus_conf.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_bus_conf.glitch_ignore_cnt = 7;
    i2c_bus_conf.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    ret = i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C master bus init returned: %s", esp_err_to_name(ret));
        return false;
    }

    // 3. Initialize SCCB interface for OV5647 (Address 0x36)
    sccb_i2c_config_t sccb_conf = {};
    sccb_conf.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    sccb_conf.device_address = OV5647_SCCB_ADDR;
    sccb_conf.scl_speed_hz = 100000;
    sccb_conf.addr_bits_width = 16;
    sccb_conf.val_bits_width = 8;

    esp_sccb_io_handle_t sccb_handle = NULL;
    ret = sccb_new_i2c_io(i2c_bus_handle, &sccb_conf, &sccb_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SCCB interface creation failed: %s", esp_err_to_name(ret));
        return false;
    }

    // 4. Detect OV5647 Sensor
    esp_cam_sensor_config_t cam_config = {};
    cam_config.sccb_handle = sccb_handle;
    cam_config.reset_pin = -1;
    cam_config.pwdn_pin = -1;
    cam_config.xclk_pin = -1;
    cam_config.sensor_port = ESP_CAM_SENSOR_MIPI_CSI;

    esp_cam_sensor_device_t *sensor = ov5647_detect(&cam_config);
    if (!sensor) {
        ESP_LOGW(TAG, "OV5647 camera sensor NOT detected on I2C address 0x%02X.", OV5647_SCCB_ADDR);
        return false;
    }
    ESP_LOGI(TAG, "OV5647 Raspberry Pi camera rev 1.3 detected successfully!");

    // 5. Query and select format
    esp_cam_sensor_format_array_t fmt_array = {};
    esp_cam_sensor_query_format(sensor, &fmt_array);
    const esp_cam_sensor_format_t *target_fmt = NULL;
    for (int i = 0; i < fmt_array.count; i++) {
        ESP_LOGI(TAG, "Supported sensor format [%d]: %s", i, fmt_array.format_array[i].name);
        if (strstr(fmt_array.format_array[i].name, "800x640") != NULL ||
            strstr(fmt_array.format_array[i].name, "raw8") != NULL) {
            target_fmt = &fmt_array.format_array[i];
        }
    }
    if (!target_fmt && fmt_array.count > 0) {
        target_fmt = &fmt_array.format_array[0];
    }
    if (target_fmt) {
        ESP_LOGI(TAG, "Setting sensor format: %s", target_fmt->name);
        esp_cam_sensor_set_format(sensor, target_fmt);
    }

    s_sccb_handle = sccb_handle;

    // Apply OV5647 Exposure, Analog Gain (AGC/AEC), AWB, and BLC calibration registers
    static const struct {
        uint16_t reg;
        uint8_t val;
    } ov5647_calib_regs[] = {
        {0x5000, 0x07}, // Enable BPC + WPC + AWB gain (keep LENC bit 7 OFF so no radial vignette!)
        {0x5001, 0x01}, // Enable AWB
        {0x5002, 0x40}, // Disable banding filter clamp
        {0x5180, 0x00}, // Auto AWB gain
        {0x380e, 0x0d}, // Increase VTS to 3500 lines (0x0dac) for 3.5x longer indoor exposure (~14 fps)
        {0x380f, 0xac},
        {0x3a08, 0x01}, // Restore 50Hz/60Hz band steps (295 / 246 lines)
        {0x3a09, 0x27},
        {0x3a0a, 0x00},
        {0x3a0b, 0xf6},
        {0x3a0d, 0x0d}, // Max 60Hz bands: 13 * 246 = 3198 lines (< VTS 3500!)
        {0x3a0e, 0x0b}, // Max 50Hz bands: 11 * 295 = 3245 lines (< VTS 3500!)
        {0x3a01, 0x01},
        {0x3a02, 0x0d}, // Max exposure 60Hz = 3328 lines (0x0d00 < VTS 3500!)
        {0x3a03, 0x00},
        {0x3a14, 0x0d}, // Max exposure 50Hz = 3328 lines (0x0d00 < VTS 3500!)
        {0x3a15, 0x00},
        {0x3a18, 0x03}, // Max AGC gain ceiling = 0x03ff (64x)
        {0x3a19, 0xff},
        {0x3a0f, 0x78}, // Bright AE target window
        {0x3a10, 0x68},
        {0x3a1b, 0x78},
        {0x3a1e, 0x68},
        {0x3a11, 0xd0},
        {0x3a1f, 0x40},
        {0x3500, 0x00}, // Initial exposure = 3200 lines (0x0c80 << 4 = 0x00c800 < VTS 3500!)
        {0x3501, 0xc8},
        {0x3502, 0x00},
        {0x350a, 0x01}, // Initial analog gain = 31x (0x01f0)
        {0x350b, 0xf0},
        {0x3503, 0x00}, // Enable full Auto Exposure (AEC) + Auto Gain (AGC)
        {0x4000, 0x00}, // Disable dark BLC subtraction in 8-bit mode
        {0x4001, 0x02},
        {0x4004, 0x02},
    };
    for (size_t i = 0; i < sizeof(ov5647_calib_regs) / sizeof(ov5647_calib_regs[0]); i++) {
        esp_sccb_transmit_reg_a16v8(sccb_handle, ov5647_calib_regs[i].reg, ov5647_calib_regs[i].val);
    }

    int stream_flag = 1;
    esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_flag);

    // 6. Allocate frame buffer in PSRAM strictly aligned to 128 bytes cache line for ESP32-P4
    if (!s_frame_sem) {
        s_frame_sem = xSemaphoreCreateBinary();
    }

    s_cam_aligned_size = (CAM_BUFFER_SIZE + 127) & ~127;
    s_cam_buffer = (uint8_t *)heap_caps_aligned_alloc(128, s_cam_aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_thumb_rgb888 = (uint8_t *)heap_caps_malloc(THUMB_RGB888_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_b64_buffer = (char *)heap_caps_malloc(THUMB_RGB888_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cam_buffer || !s_thumb_rgb888 || !s_b64_buffer) {
        ESP_LOGE(TAG, "Could not allocate camera PSRAM buffers");
        return false;
    }

    s_active_trans.buffer = s_cam_buffer;
    s_active_trans.buflen = s_cam_aligned_size;
    s_active_trans.received_size = s_cam_aligned_size;

    // 7. Initialize MIPI-CSI Controller (Hardware RAW8 -> RGB565)
    esp_cam_ctlr_csi_config_t csi_config = {};
    csi_config.ctlr_id = 0;
    csi_config.h_res = CAM_MIPI_CSI_HRES;
    csi_config.v_res = CAM_MIPI_CSI_VRES;
    csi_config.data_lane_num = 2;
    csi_config.lane_bit_rate_mbps = CAM_LANE_BITRATE_MBPS;
    csi_config.input_data_color_type = CAM_CTLR_COLOR_RAW8;
    csi_config.output_data_color_type = CAM_CTLR_COLOR_RGB565;
    csi_config.queue_items = 1;
    csi_config.byte_swap_en = false;
    csi_config.bk_buffer_dis = false;

    ret = esp_cam_new_csi_ctlr(&csi_config, &s_cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_cam_new_csi_ctlr failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Register required frame event callbacks
    esp_cam_ctlr_evt_cbs_t cbs = {};
    cbs.on_get_new_trans = s_camera_get_new_vb;
    cbs.on_trans_finished = s_camera_get_finished_trans;
    ret = esp_cam_ctlr_register_event_callbacks(s_cam_handle, &cbs, &s_active_trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register_event_callbacks failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_cam_ctlr_enable(s_cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_cam_ctlr_enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    // 8. Initialize Hardware ISP Processor
    isp_proc_handle_t isp_proc = NULL;
    esp_isp_processor_cfg_t isp_config = {};
    isp_config.clk_hz = 80 * 1000 * 1000;
    isp_config.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    isp_config.input_data_color_type = ISP_COLOR_RAW8;
    isp_config.output_data_color_type = ISP_COLOR_RGB565;
    isp_config.has_line_start_packet = false;
    isp_config.has_line_end_packet = false;
    isp_config.h_res = CAM_MIPI_CSI_HRES;
    isp_config.v_res = CAM_MIPI_CSI_VRES;

    ret = esp_isp_new_processor(&isp_config, &isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_isp_new_processor failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_isp_enable(isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_isp_enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Init frame buffer cache state
    memset(s_cam_buffer, 0, s_cam_aligned_size);
    esp_cache_msync((void *)s_cam_buffer, s_cam_aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ret = esp_cam_ctlr_start(s_cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_cam_ctlr_start failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "MIPI-CSI Live Camera stream is ACTIVE with hardware ISP!");
    return true;
}

static void run_camera_loop(ESPDetDetect *detect)
{
    ESP_LOGI(TAG, "Running LIVE CAMERA detection loop (OV5647 MIPI-CSI)...");
    uint32_t frame_index = 0;

    while (true) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Waiting for camera frame from CSI controller...");
            continue;
        }

        // Invalidate CPU cache so we read fresh DMA pixels from PSRAM
        esp_cache_msync((void *)s_cam_buffer, s_cam_aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        // 1. Area-average downsample 800x640 RGB565LE -> 266x212 RGB888
        uint16_t *src16 = (uint16_t *)s_cam_buffer;
        uint8_t *dst8 = s_thumb_rgb888;
        uint8_t min_r = 255, max_r = 0;
        uint8_t min_g = 255, max_g = 0;
        uint8_t min_b = 255, max_b = 0;
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0;

        for (int y = 0; y < THUMB_H; y++) {
            int base_y = y * 3;
            for (int x = 0; x < THUMB_W; x++) {
                int base_x = x * 3;
                uint32_t acc_r = 0, acc_g = 0, acc_b = 0;
                for (int dy = 0; dy < 2; dy++) {
                    const uint16_t *row = src16 + (base_y + dy) * CAM_MIPI_CSI_HRES + base_x;
                    for (int dx = 0; dx < 2; dx++) {
                        uint16_t p = row[dx];
                        acc_r += ((p >> 11) & 0x1F) << 3;
                        acc_g += ((p >> 5) & 0x3F) << 2;
                        acc_b += (p & 0x1F) << 3;
                    }
                }
                uint8_t r = (uint8_t)(acc_r >> 2);
                uint8_t g = (uint8_t)(acc_g >> 2);
                uint8_t b = (uint8_t)(acc_b >> 2);

                if (r < min_r) min_r = r;
                if (r > max_r) max_r = r;
                if (g < min_g) min_g = g;
                if (g > max_g) max_g = g;
                if (b < min_b) min_b = b;
                if (b > max_b) max_b = b;

                sum_r += r;
                sum_g += g;
                sum_b += b;
                *dst8++ = r;
                *dst8++ = g;
                *dst8++ = b;
            }
        }

        uint32_t num_thumb_px = THUMB_W * THUMB_H;
        uint32_t mean_r = sum_r / num_thumb_px;
        uint32_t mean_g = sum_g / num_thumb_px;
        uint32_t mean_b = sum_b / num_thumb_px;
        uint32_t mean_all = (mean_r + mean_g + mean_b) / 3;

        uint8_t exp0 = 0, exp1 = 0, exp2 = 0, agc0 = 0, agc1 = 0;
        if (s_sccb_handle) {
            esp_sccb_transmit_receive_reg_a16v8(s_sccb_handle, 0x3500, &exp0);
            esp_sccb_transmit_receive_reg_a16v8(s_sccb_handle, 0x3501, &exp1);
            esp_sccb_transmit_receive_reg_a16v8(s_sccb_handle, 0x3502, &exp2);
            esp_sccb_transmit_receive_reg_a16v8(s_sccb_handle, 0x350a, &agc0);
            esp_sccb_transmit_receive_reg_a16v8(s_sccb_handle, 0x350b, &agc1);
        }
        ESP_LOGI(TAG, "Frame #%lu raw stats: R[%u..%u] G[%u..%u] B[%u..%u] mean=%lu | exp=0x%02x%02x%02x agc=0x%02x%02x",
                 (unsigned long)frame_index, min_r, max_r, min_g, max_g, min_b, max_b,
                 (unsigned long)mean_all, exp0, exp1, exp2, agc0, agc1);

        // 2. Natural Gray-World White Balance + Capped Gain (<= 3.5x) so colors stay natural without amplifying noise
        uint32_t ped_r = (min_r * 3) / 4;
        uint32_t ped_g = (min_g * 3) / 4;
        uint32_t ped_b = (min_b * 3) / 4;
        uint32_t net_r = (mean_r > ped_r) ? (mean_r - ped_r) : 1;
        uint32_t net_g = (mean_g > ped_g) ? (mean_g - ped_g) : 1;
        uint32_t net_b = (mean_b > ped_b) ? (mean_b - ped_b) : 1;
        uint32_t target_mean = 115;

        // Gain in 8.8 fixed point, capped at 3.5x (896 in Q8) to prevent shadow noise explosion
        uint32_t gain_r_q8 = (target_mean * 256) / net_r;
        uint32_t gain_g_q8 = (target_mean * 256) / net_g;
        uint32_t gain_b_q8 = (target_mean * 256) / net_b;
        if (gain_r_q8 > 896) gain_r_q8 = 896;
        if (gain_g_q8 > 896) gain_g_q8 = 896;
        if (gain_b_q8 > 896) gain_b_q8 = 896;

        for (size_t i = 0; i < THUMB_RGB888_SIZE; i += 3) {
            uint32_t r = s_thumb_rgb888[i];
            uint32_t g = s_thumb_rgb888[i + 1];
            uint32_t b = s_thumb_rgb888[i + 2];

            r = (r > ped_r) ? (((r - ped_r) * gain_r_q8) >> 8) : 0;
            g = (g > ped_g) ? (((g - ped_g) * gain_g_q8) >> 8) : 0;
            b = (b > ped_b) ? (((b - ped_b) * gain_b_q8) >> 8) : 0;

            s_thumb_rgb888[i]     = (uint8_t)(r > 255 ? 255 : r);
            s_thumb_rgb888[i + 1] = (uint8_t)(g > 255 ? 255 : g);
            s_thumb_rgb888[i + 2] = (uint8_t)(b > 255 ? 255 : b);
        }

        // Apply same natural white balance to full frame via fast LUTs for ESPDet Pico
        uint16_t lut_r[32], lut_g[64], lut_b[32];
        for (int i = 0; i < 32; i++) {
            uint32_t v = i << 3;
            v = (v > ped_r) ? (((v - ped_r) * gain_r_q8) >> 8) : 0;
            if (v > 255) v = 255;
            lut_r[i] = (uint16_t)((v >> 3) << 11);
        }
        for (int i = 0; i < 64; i++) {
            uint32_t v = i << 2;
            v = (v > ped_g) ? (((v - ped_g) * gain_g_q8) >> 8) : 0;
            if (v > 255) v = 255;
            lut_g[i] = (uint16_t)((v >> 2) << 5);
        }
        for (int i = 0; i < 32; i++) {
            uint32_t v = i << 3;
            v = (v > ped_b) ? (((v - ped_b) * gain_b_q8) >> 8) : 0;
            if (v > 255) v = 255;
            lut_b[i] = (uint16_t)(v >> 3);
        }

        size_t num_px = CAM_MIPI_CSI_HRES * CAM_MIPI_CSI_VRES;
        for (size_t i = 0; i < num_px; i++) {
            uint16_t p = src16[i];
            src16[i] = lut_r[(p >> 11) & 0x1F] | lut_g[(p >> 5) & 0x3F] | lut_b[p & 0x1F];
        }

        dl::image::img_t img;
        img.data = s_cam_buffer;
        img.width = CAM_MIPI_CSI_HRES;
        img.height = CAM_MIPI_CSI_VRES;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

        // 3. Run neural network inference on balanced frame
        int64_t t_inf = esp_timer_get_time();
        auto &results = detect->run(img);
        int inference_ms = (int)((esp_timer_get_time() - t_inf) / 1000);

        dl::image::img_t thumb_img;
        thumb_img.data = s_thumb_rgb888;
        thumb_img.width = THUMB_W;
        thumb_img.height = THUMB_H;
        thumb_img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

        dl::image::jpeg_img_t jpeg = dl::image::sw_encode_jpeg_base(thumb_img, 55);
        s_b64_buffer[0] = '\0';
        if (jpeg.data && jpeg.data_len > 0) {
            base64_encode((const uint8_t *)jpeg.data, jpeg.data_len, s_b64_buffer);
            heap_caps_free(jpeg.data);
        }

        // Output JSON telemetry + Live Base64 JPEG for Dashboard
        printf("\n>>>JSON_START<<<\n");
        printf("{\n");
        printf("  \"source\": \"camera\",\n");
        printf("  \"frame\": %lu,\n", (unsigned long)frame_index++);
        printf("  \"image\": \"live_camera_feed.jpg\",\n");
        printf("  \"width\": %d, \"height\": %d,\n", CAM_MIPI_CSI_HRES, CAM_MIPI_CSI_VRES);
        printf("  \"thumb_width\": %d, \"thumb_height\": %d,\n", THUMB_W, THUMB_H);
        printf("  \"inference_ms\": %d,\n", inference_ms);
        printf("  \"image_b64\": \"%s\",\n", s_b64_buffer);
        printf("  \"detections\": [\n");

        size_t idx = 0;
        for (auto it = results.begin(); it != results.end(); ++it, ++idx) {
            const auto &res = *it;
            printf("    {\"category\": %d, \"score\": %f, \"box\": [%d, %d, %d, %d]}%s\n",
                   res.category, res.score, res.box[0], res.box[1], res.box[2], res.box[3],
                   (idx == results.size() - 1) ? "" : ",");
        }
        printf("  ]\n");
        printf("}\n");
        printf(">>>JSON_END<<<\n\n");

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void run_spiffs_loop(ESPDetDetect *detect)
{
    ESP_LOGI(TAG, "Running SPIFFS image validation loop...");

    while (true) {
        DIR* dir = opendir("/spiffs");
        if (dir == NULL) {
            ESP_LOGE(TAG, "Failed to open /spiffs directory");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (is_jpeg_file(ent->d_name)) {
                char filepath[300];
                snprintf(filepath, sizeof(filepath), "/spiffs/%s", ent->d_name);
                ESP_LOGI(TAG, "Processing %s", filepath);

                dl::image::jpeg_img_t jpeg = dl::image::read_jpeg(filepath);
                if (jpeg.data == nullptr) {
                    ESP_LOGE(TAG, "Failed to read %s", filepath);
                    continue;
                }

                int64_t t_dec = esp_timer_get_time();
                dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
                heap_caps_free(jpeg.data);
                if (img.data == nullptr) {
                    ESP_LOGE(TAG, "Failed to decode %s", filepath);
                    continue;
                }

                int64_t t_inf = esp_timer_get_time();
                auto &results = detect->run(img);
                int64_t t_end = esp_timer_get_time();

                printf("\n>>>JSON_START<<<\n");
                printf("{\n");
                printf("  \"source\": \"spiffs\",\n");
                printf("  \"image\": \"%s\",\n", ent->d_name);
                printf("  \"width\": %d, \"height\": %d,\n", img.width, img.height);
                printf("  \"decode_ms\": %d, \"inference_ms\": %d,\n",
                       (int)((t_inf - t_dec) / 1000), (int)((t_end - t_inf) / 1000));
                printf("  \"detections\": [\n");

                size_t idx = 0;
                for (auto it = results.begin(); it != results.end(); ++it, ++idx) {
                    const auto &res = *it;
                    printf("    {\"category\": %d, \"score\": %f, \"box\": [%d, %d, %d, %d]}%s\n",
                           res.category, res.score, res.box[0], res.box[1], res.box[2], res.box[3],
                           (idx == results.size() - 1) ? "" : ",");
                }
                printf("  ]\n");
                printf("}\n");
                printf(">>>JSON_END<<<\n\n");

                heap_caps_free(img.data);

                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        closedir(dir);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

extern "C" void app_main(void)
{
    // Switch UART0 console to 921600 baud for high-speed live video + telemetry streaming
    soc_module_clk_t uart_sclk = SOC_MOD_CLK_XTAL;
    uart_ll_get_sclk(UART_LL_GET_HW(0), &uart_sclk);
    uint32_t sclk_freq = (uart_sclk == (soc_module_clk_t)UART_SCLK_PLL_F80M) ? 80000000 : 40000000;
#undef uart_ll_set_baudrate
    uart_ll_set_baudrate(UART_LL_GET_HW(0), 921600, sclk_freq);

    ESP_LOGI(TAG, "=== Obstacle & Person Detection System (ESP32-P4) ===");

    // Mount filesystem for validation dataset
    mount_spiffs();

    // Load ESP-DL Neural Network Model
    ESP_LOGI(TAG, "Loading ESP-DL Pico 224x224 Detection Model...");
    int64_t t_load = esp_timer_get_time();
    // lazy_load = false: load the model now instead of on the first run().
    ESPDetDetect *detect = new ESPDetDetect(ESPDetDetect::ESPDET_PICO_224_224_OBSTACLE_PERSON, false);
    ESP_LOGI(TAG, "Model loaded in %lld ms", (esp_timer_get_time() - t_load) / 1000);

    // Attempt to start the live MIPI-CSI Raspberry Pi Camera (OV5647)
    bool camera_online = init_camera();

    if (camera_online) {
        ESP_LOGI(TAG, "Camera detected: Starting LIVE REAL-TIME CAMERA pipeline!");
        run_camera_loop(detect);
    } else {
        ESP_LOGW(TAG, "Camera not detected on MIPI-CSI connector. Gracefully falling back to SPIFFS validation images.");
        run_spiffs_loop(detect);
    }

    delete detect;
}
