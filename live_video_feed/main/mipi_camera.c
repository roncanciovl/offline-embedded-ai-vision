#include "mipi_camera.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_sccb_i2c.h"
#include "esp_ldo_regulator.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_cache.h"
#include "hal/cam_ctlr_types.h"

static const char *TAG = "mipi_camera";

// Hardware-specific I2C pins for Guition ESP32-P4
#define I2C_SDA_IO 7
#define I2C_SCL_IO 8
// We allocate RAW8 size. 1 byte per pixel.
#define FRAME_SIZE (CAM_WIDTH * CAM_HEIGHT)

static esp_cam_ctlr_handle_t s_cam_ctlr = NULL;
static esp_cam_ctlr_trans_t s_trans = {0};
static void *s_frame_buffer = NULL;
static size_t s_frame_size = 0;

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    trans->buffer = s_frame_buffer;
    trans->buflen = s_frame_size;
    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    return false;
}

esp_err_t mipi_camera_init(void)
{
    // Power on MIPI PHY LDO
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    esp_err_t ret_ldo = esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);
    if (ret_ldo != ESP_OK) {
        ESP_LOGE(TAG, "Failed to power on MIPI PHY LDO!");
        // We do not return error here because if it's already acquired elsewhere it might return an error but still be on.
    }

    ESP_LOGI(TAG, "Initializing custom P4 MIPI CSI driver for OV5647...");
    esp_err_t ret;

    // 1. Init I2C
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    ret = i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle);
    if (ret != ESP_OK) return ret;

    // 2. Init SCCB
    sccb_i2c_config_t sccb_config = {
        .scl_speed_hz = 100000,
        .device_address = 0x36, // OV5647 default 7-bit address
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    };
    esp_sccb_io_handle_t sccb_handle = NULL;
    ret = sccb_new_i2c_io(i2c_bus_handle, &sccb_config, &sccb_handle);
    if (ret != ESP_OK) return ret;

    esp_cam_sensor_config_t cam_config = {
        .sccb_handle = sccb_handle,
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };
    
    // Auto-detect camera (should find OV5647)
    esp_cam_sensor_device_t *cam = NULL;
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start; p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        cam = (*(p->detect))(&cam_config);
        if (cam) break;
    }
    if (!cam) {
        ESP_LOGE(TAG, "Failed to detect camera sensor");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera sensor detected");

    // Configure the camera registers with the default format
    ret = esp_cam_sensor_set_format(cam, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set camera format");
        return ret;
    }

    // Get the configured format
    esp_cam_sensor_format_t cam_format;
    ESP_ERROR_CHECK(esp_cam_sensor_get_format(cam, &cam_format));
    ESP_LOGI(TAG, "Driver reports format: %dx%d", cam_format.width, cam_format.height);
    ESP_LOGI(TAG, "Driver reports mipi_clk: %ld Hz", cam_format.mipi_info.mipi_clk);
    ESP_LOGI(TAG, "Driver reports lane_num: %lu", (unsigned long)cam_format.mipi_info.lane_num);

    // Test pattern to see if the MIPI link works at all
    int test_pattern_en = 0; // 0 = Disable
    esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_TEST_PATTERN, &test_pattern_en);
    ESP_LOGI(TAG, "Camera test pattern DISABLED");

    // 3. Allocate a massive Frame Buffer in PSRAM to prevent any size mismatches
    s_frame_size = 2000000; // 2MB is plenty for 800x800 even at 16-bit
    s_frame_buffer = heap_caps_aligned_alloc(64, s_frame_size, MALLOC_CAP_SPIRAM);
    if (!s_frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    // Zero-initialize to clearly see if DMA writes anything (instead of PSRAM noise)
    memset(s_frame_buffer, 0, s_frame_size);

    s_trans.buffer = s_frame_buffer;
    s_trans.buflen = s_frame_size;
    
    cam_ctlr_color_t input_color = CAM_CTLR_COLOR_RAW8;
    if (cam_format.format == ESP_CAM_SENSOR_PIXFORMAT_RAW10) {
        input_color = CAM_CTLR_COLOR_RAW10;
        ESP_LOGI(TAG, "Configuring CSI for RAW10 input -> RAW8 output");
    } else {
        ESP_LOGI(TAG, "Configuring CSI for RAW8 input");
    }

    // CSI Config
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = cam_format.width,
        .v_res = cam_format.height, // 800 (Perfectly aligned to 64 bytes)
        .lane_bit_rate_mbps = 400, // Exact bitrate output by the camera's PLL
        .input_data_color_type = input_color, 
        .output_data_color_type = CAM_CTLR_COLOR_RAW8, // Must match camera's physical output size!
        .data_lane_num = cam_format.mipi_info.lane_num,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    ret = esp_cam_new_csi_ctlr(&csi_config, &s_cam_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New CSI controller fail");
        return ret;
    }
    
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    esp_cam_ctlr_register_event_callbacks(s_cam_ctlr, &cbs, &s_trans);
    
    // Start CSI controller FIRST so it is ready to receive
    esp_cam_ctlr_enable(s_cam_ctlr);
    ret = esp_cam_ctlr_start(s_cam_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI Start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // NOW start the camera stream
    int enable_flag = 1;
    ret = esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start stream fail");
        return ret;
    }
    
    ESP_LOGI(TAG, "Camera initialization complete");
    return ESP_OK;
}

esp_err_t mipi_camera_capture(void **out_buffer, size_t *out_size)
{
    // Wait for the DMA End-Of-Frame interrupt by calling esp_cam_ctlr_receive
    esp_err_t ret = esp_cam_ctlr_receive(s_cam_ctlr, &s_trans, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera capture timeout! Frame didn't finish.");
        return ret;
    }

    // Invalidate the CPU cache so we can read the newly written DMA data!
    esp_cache_msync((void *)s_frame_buffer, s_frame_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    *out_buffer = s_frame_buffer;
    *out_size = s_frame_size;
    return ESP_OK;
}
