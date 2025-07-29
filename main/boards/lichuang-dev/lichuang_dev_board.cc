#include "wifi_board.h"
#include "audio_codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include "esp32_camera.h"
#include "mcp_server.h"
//#include "no_audio_codec.h"
#include "dummy_audio_codec.h"
#include "audio_codecs/santa_audio_codec.h"
#include "audio_codecs/no_audio_codec.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "LichuangDevBoard"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class lichuangcodec : public SantaAudioCodec  {
private:    

public:
    lichuangcodec(i2c_master_bus_handle_t i2c_bus, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din, uint8_t es7210_addr, bool input_reference)
        : SantaAudioCodec(i2c_bus, input_sample_rate, output_sample_rate,
                             mclk,  bclk,  ws,  dout,  din, es7210_addr, input_reference) {}

    virtual void EnableOutput(bool enable) override {
        SantaAudioCodec::EnableOutput(enable);
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Button wake_button_;
    LcdDisplay* display_;
    Esp32Camera* camera_;
    
    // Add motor state tracking
    bool motors_initialized_ = false;
    bool motors_enabled_ = false;

    void InitializeMotors() {
        ESP_LOGI(TAG, "Initializing LEDC for motor control...");
        
        // First, configure all motor control pins as outputs and set them to safe states
        gpio_config_t gpio_conf = {
            .pin_bit_mask = (1ULL << HEAD_DIR_PIN) | (1ULL << HIP_FWD_PIN) | (1ULL << HIP_BWD_PIN) | (1ULL << HEAD_PWM_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&gpio_conf);
        
        // Set all pins to safe states (LOW) before configuring PWM
        gpio_set_level(HEAD_DIR_PIN, 0);
        gpio_set_level(HEAD_PWM_PIN, 0);
        gpio_set_level(HIP_FWD_PIN, 0);
        gpio_set_level(HIP_BWD_PIN, 0);
        
        ESP_LOGI(TAG, "Set all motor pins to safe states");
        
        // Configure LEDC timer
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LEDC_MODE,
            .duty_resolution  = LEDC_DUTY_RES,
            .timer_num        = LEDC_TIMER,
            .freq_hz          = LEDC_FREQUENCY,
            .clk_cfg          = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

        // Configure PWM channels with 0 duty cycle
        ledc_channel_config_t channels[] = {
            {
                .gpio_num       = HEAD_PWM_PIN,
                .speed_mode     = LEDC_MODE,
                .channel        = HEAD_PWM_CHANNEL,
                .intr_type      = LEDC_INTR_DISABLE,
                .timer_sel      = LEDC_TIMER,
                .duty           = 0,
                .hpoint         = 0
            },
            {
                .gpio_num       = HIP_FWD_PIN,
                .speed_mode     = LEDC_MODE,
                .channel        = HIP_FWD_CHANNEL,
                .intr_type      = LEDC_INTR_DISABLE,
                .timer_sel      = LEDC_TIMER,
                .duty           = 0,
                .hpoint         = 0
            },
            {
                .gpio_num       = HIP_BWD_PIN,
                .speed_mode     = LEDC_MODE,
                .channel        = HIP_BWD_CHANNEL,
                .intr_type      = LEDC_INTR_DISABLE,
                .timer_sel      = LEDC_TIMER,
                .duty           = 0,
                .hpoint         = 0
            }
        };
        
        for (int i = 0; i < 3; i++) {
            ESP_ERROR_CHECK(ledc_channel_config(&channels[i]));
            // Explicitly set duty to 0 and update
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, channels[i].channel, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, channels[i].channel));
            ESP_LOGI(TAG, "Configured PWM channel %d on GPIO %d with 0 duty", channels[i].channel, channels[i].gpio_num);
        }

        motors_initialized_ = true;
        motors_enabled_ = false;  // Motors are disabled by default
        
        // Ensure all motors are stopped
        ForceStopAllMotors();
        
        ESP_LOGI(TAG, "Motor initialization complete - all motors stopped and disabled");
    }

    // Add this new method for forceful stopping
    void ForceStopAllMotors() {
        if (!motors_initialized_) {
            return;  // Silent return if not initialized
        }
        
        // Check if motors are actually running before stopping
        uint32_t head_duty = ledc_get_duty(LEDC_MODE, HEAD_PWM_CHANNEL);
        uint32_t hip_fwd_duty = ledc_get_duty(LEDC_MODE, HIP_FWD_CHANNEL);
        uint32_t hip_bwd_duty = ledc_get_duty(LEDC_MODE, HIP_BWD_CHANNEL);
        
        bool motors_were_running = (head_duty > 0 || hip_fwd_duty > 0 || hip_bwd_duty > 0);
        
        // Stop all motors
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HEAD_PWM_CHANNEL, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HEAD_PWM_CHANNEL));
        ESP_ERROR_CHECK(gpio_set_level(HEAD_DIR_PIN, 0));
        
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_FWD_CHANNEL, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_FWD_CHANNEL));
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_BWD_CHANNEL, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_BWD_CHANNEL));
        
        // Set GPIO pins directly to LOW as backup
        gpio_set_level(HEAD_PWM_PIN, 0);
        gpio_set_level(HIP_FWD_PIN, 0);
        gpio_set_level(HIP_BWD_PIN, 0);
        
        motors_enabled_ = false;
        
        // Only log if motors were actually running
        if (motors_were_running) {
            ESP_LOGI(TAG, "Motors were running unexpectedly - force stopped");
        }
    }

    // Add motor enable/disable control
    void EnableMotors(bool enable) {
        motors_enabled_ = enable;
        ESP_LOGI(TAG, "Motors %s", enable ? "ENABLED" : "DISABLED");
        
        if (!enable) {
            ForceStopAllMotors();
        }
    }

    void SetHeadSpeed(int speed) {
        if (!motors_enabled_) {
            ESP_LOGW(TAG, "Motors disabled, ignoring head speed command: %d", speed);
            return;
        }
        
        ESP_LOGI(TAG, "Setting head speed to %d", speed);
        
        if (speed > 0) {
            uint32_t duty = (uint32_t)((abs(speed) * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HEAD_PWM_CHANNEL, duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HEAD_PWM_CHANNEL));
            ESP_ERROR_CHECK(gpio_set_level(HEAD_DIR_PIN, 1));
            ESP_LOGI(TAG, "Head forward: PWM duty=%lu, DIR=1", duty);
        } else if (speed < 0) {
            uint32_t duty = (uint32_t)((abs(speed) * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HEAD_PWM_CHANNEL, duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HEAD_PWM_CHANNEL));
            ESP_ERROR_CHECK(gpio_set_level(HEAD_DIR_PIN, 0));
            ESP_LOGI(TAG, "Head backward: PWM duty=%lu, DIR=0", duty);
        } else {
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HEAD_PWM_CHANNEL, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HEAD_PWM_CHANNEL));
            ESP_ERROR_CHECK(gpio_set_level(HEAD_DIR_PIN, 0));
            ESP_LOGI(TAG, "Head stopped");
        }
    }

    void SetHipSpeed(int speed) {
        if (!motors_enabled_) {
            ESP_LOGW(TAG, "Motors disabled, ignoring hip speed command: %d", speed);
            return;
        }
        
        ESP_LOGI(TAG, "Setting hip speed to %d", speed);
        
        if (speed > 0) {
            uint32_t duty = (uint32_t)((abs(speed) * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_FWD_CHANNEL, duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_FWD_CHANNEL));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_BWD_CHANNEL, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_BWD_CHANNEL));
            ESP_LOGI(TAG, "Hip forward: FWD duty=%lu, BWD duty=0", duty);
        } else if (speed < 0) {
            uint32_t duty = (uint32_t)((abs(speed) * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_FWD_CHANNEL, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_FWD_CHANNEL));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_BWD_CHANNEL, duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_BWD_CHANNEL));
            ESP_LOGI(TAG, "Hip backward: FWD duty=0, BWD duty=%lu", duty);
        } else {
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_FWD_CHANNEL, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_FWD_CHANNEL));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, HIP_BWD_CHANNEL, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, HIP_BWD_CHANNEL));
            ESP_LOGI(TAG, "Hip stopped");
        }
    }

    void SparkBotDance() {
        ESP_LOGI(TAG, "Starting original dance sequence!");
        EnableMotors(true);  // Enable motors for dance
        
        int cnt = 0;
        while (1) {
            cnt++;
            if (cnt == 5) {
                SetHeadSpeed(0); 
                SetHipSpeed(0);
                break;
            }
             
            for (int i = 0; i < 15; i++) {
                if (i <= 5) {
                    SetHeadSpeed(80); 
                    vTaskDelay(300 / portTICK_PERIOD_MS);
                } else {
                    if (i == 6) SetHeadSpeed(0);
                    if (i % 2 == 0) {
                        SetHipSpeed(100);
                    } else {
                        SetHipSpeed(0);
                    }
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                }
            }
            SetHipSpeed(0);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        
        EnableMotors(false);  // Disable motors after dance
        ESP_LOGI(TAG, "Dance sequence complete!");
    }

    void HeadShakeOnly() {
        ESP_LOGI(TAG, "Starting smooth randomized head shake sequence!");
        EnableMotors(true);  // Enable motors for head shake
        
        for (int i = 0; i < 100; i++) {
            int speed1 = 90 + (esp_random() % 11);
            int speed2 = 90 + (esp_random() % 11);
            
            SetHeadSpeed(speed1);
            vTaskDelay((35 + esp_random() % 11) / portTICK_PERIOD_MS);
            
            SetHeadSpeed(-speed2);
            vTaskDelay((35 + esp_random() % 11) / portTICK_PERIOD_MS);
        }
        SetHeadSpeed(0);
        EnableMotors(false);  // Disable motors after head shake
        
        ESP_LOGI(TAG, "Head shake complete!");
    }

    void HipShakeOnly() {
        ESP_LOGI(TAG, "Starting smooth hip shake sequence!");
        EnableMotors(true);  // Enable motors for hip shake
        
        for (int i = 0; i < 8; i++) {
            SetHipSpeed(70);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            SetHipSpeed(0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            SetHipSpeed(-70);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            SetHipSpeed(0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        SetHipSpeed(0);
        EnableMotors(false);  // Disable motors after hip shake
        
        ESP_LOGI(TAG, "Hip shake complete!");
    }

    // Add a periodic task to monitor and stop unwanted motor movement
    static void MotorWatchdogTask(void* param) {
        LichuangDevBoard* board = (LichuangDevBoard*)param;
        while (1) {
            if (!board->motors_enabled_) {
                board->ForceStopAllMotors();
            }
            vTaskDelay(2000 / portTICK_PERIOD_MS);  // Check every 2 seconds instead of 500ms
        }
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        mcp_server.AddTool("self.chassis.dance", "跳舞", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Dance command received");
            // Create a task to run the dance so it doesn't block the MCP response
            auto dance_task = [](void* param) {
                ((LichuangDevBoard*)param)->SparkBotDance();
                vTaskDelete(NULL);
            };
            xTaskCreate(dance_task, "dance_task", 4096, this, 5, NULL);
            return true;
        });
        
        mcp_server.AddTool("self_chassis_shake_body", "摇头", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Head shake command received");
            auto head_task = [](void* param) {
                ((LichuangDevBoard*)param)->HeadShakeOnly();
                vTaskDelete(NULL);
            };
            xTaskCreate(head_task, "head_task", 4096, this, 5, NULL);
            return true;
        });
        
        mcp_server.AddTool("self_chassis_shake_hip", "摇屁股", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Hip shake command received");
            auto hip_task = [](void* param) {
                ((LichuangDevBoard*)param)->HipShakeOnly();
                vTaskDelete(NULL);
            };
            xTaskCreate(hip_task, "hip_task", 4096, this, 5, NULL);
            return true;
        });
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        // Boot button functionality
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        // Wake button with same functionality as boot button
        wake_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });

        // Add same double-click functionality to wake button
        wake_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                        .emoji_font = font_emoji_32_init(),
#else
                                        .emoji_font = font_emoji_64_init(),
#endif
                                    });
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_3;  // Changed from LEDC_CHANNEL_2 to avoid conflict with motors
        config.ledc_timer = LEDC_TIMER_1; // Changed from LEDC_TIMER_2 to avoid conflict with motors
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }
    void InitializeMotorPinsSafe() {
        ESP_LOGI(TAG, "Setting motor pins to safe states immediately...");
        
        // Configure all motor pins as outputs with safe states IMMEDIATELY
        gpio_config_t gpio_conf = {
            .pin_bit_mask = (1ULL << HEAD_DIR_PIN) | (1ULL << HIP_FWD_PIN) | 
                        (1ULL << HIP_BWD_PIN) | (1ULL << HEAD_PWM_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,      // ← CORRECT
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   // ← CORRECT  
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&gpio_conf);
        
        // Force all pins LOW immediately
        gpio_set_level(HEAD_DIR_PIN, 0);
        gpio_set_level(HEAD_PWM_PIN, 0);
        gpio_set_level(HIP_FWD_PIN, 0);
        gpio_set_level(HIP_BWD_PIN, 0);
        
        ESP_LOGI(TAG, "All motor pins forced to LOW state");
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO), wake_button_(WAKE_BUTTON_GPIO) {
        InitializeMotorPinsSafe(); 
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeCamera();
        // InitializeMotors();
        InitializeTools();

        // Start motor watchdog task
        // xTaskCreate(MotorWatchdogTask, "motor_watchdog", 2048, this, 1, NULL);

        // Ensure motors are stopped and disabled after all initialization
        ForceStopAllMotors();
        EnableMotors(false);


#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
#endif
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static lichuangcodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(LichuangDevBoard);