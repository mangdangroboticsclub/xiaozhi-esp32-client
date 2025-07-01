#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>

#define TAG "TurtleBot"

// Arduino-style constants
#define TIMES_WALK 1
#define SERVO_LOOP_DELAY 1
#define STEP_DELAY 10
#define INTERPOLATION_NUM1 (2*15+1)
#define INTERPOLATION_NUM2 66

// Declare the fonts
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class TurtleBot : public WifiBoard {
private:
    int current_angles_[5] = {0, 0, 0, 0, 0};
    
    // Display components
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    
    // Arduino-style servo variables
    int servo_pins_[5] = {38, 39, 40, 41, 42};  // leftfront, leftback, rightfront, rightback, head
    int ang_[5] = {0, 0, 0, 0, 0};
    int osang_[5] = {0, -3, -3, 2, 0};  // offset angles
    int AngS_[5] = {0, 0, 0, 0, 0};     // current angle positions
    
    // Arduino timing variables
    int timewalk1_ = 100;
    int timewalk2_ = 50;
    int timeLT_ = 300;  // time for left turn
    int timeST_ = 100;  // time for right turn
    
    // Arduino interpolation array
    int angles3_[2*100+1];

    // ESP-IDF helper functions
    int constrain(int value, int min_val, int max_val) {
        if (value < min_val) return min_val;
        if (value > max_val) return max_val;
        return value;
    }

    void delay(int milliseconds) {
        vTaskDelay(pdMS_TO_TICKS(milliseconds));
    }

    // Arduino-style input_ang function
    int input_ang(int x, int y) {
        switch (x) {
            case 0: return 90 - y - osang_[0];
            case 1: return 90 - y - osang_[1];
            case 2: return 90 + y + osang_[2];
            case 3: return 90 + y + osang_[3];
            case 4: return 180 - y;
            default: return 90;
        }
    }

    // Set servo angle using LEDC PWM (ESP-IDF equivalent of servo.write())
    void servo_write(int servo_index, int angle) {
        if (servo_index >= 0 && servo_index < 5) {
            angle = constrain(angle, 0, 180);
            
            // Convert angle to PWM duty cycle (1ms-2ms pulse width for 50Hz)
            uint32_t pulse_width_us = 1000 + (angle * 1000 / 180);
            uint32_t duty = (pulse_width_us * 16384) / 20000;
            
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)servo_index, duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)servo_index));
            
            ESP_LOGD(TAG, "Servo %d: angle=%d, pulse=%luμs, duty=%lu", 
                     servo_index, angle, pulse_width_us, duty);
        }
    }

    // Initialize display I2C bus
    void InitializeDisplayI2c() {
        ESP_LOGI(TAG, "Initializing display I2C bus");
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        
        esp_err_t ret = i2c_new_master_bus(&bus_config, &display_i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display I2C bus: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }
        ESP_LOGI(TAG, "Display I2C bus initialized successfully");
    }

    // Initialize SSD1306 OLED display
    void InitializeSsd1306Display() {
        ESP_LOGI(TAG, "Initializing SSD1306 OLED display");
        
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        esp_err_t ret = esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }

        ESP_LOGI(TAG, "Installing SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ret = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create SSD1306 panel: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }

        ret = esp_lcd_panel_reset(panel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reset display: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }
        
        ret = esp_lcd_panel_init(panel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display panel: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }

        ret = esp_lcd_panel_disp_on_off(panel_, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to turn on display: %s", esp_err_to_name(ret));
            display_ = new NoDisplay();
            return;
        }

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, 
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                   {&font_puhui_14_1, &font_awesome_14_1});
        
        ESP_LOGI(TAG, "SSD1306 OLED display initialized successfully");
    }

    // Initialize servo motors using LEDC
    void InitializeServos() {
        ESP_LOGI(TAG, "Initializing 5 servo motors with LEDC");
        
        ledc_timer_config_t ledc_timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_14_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 50,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        
        for (int i = 0; i < 5; i++) {
            ledc_channel_config_t ledc_channel = {
                .gpio_num = servo_pins_[i],
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel = (ledc_channel_t)i,
                .timer_sel = LEDC_TIMER_0,
                .duty = 0,
                .hpoint = 0
            };
            ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        }
        
        ESP_LOGI(TAG, "All servos initialized");
    }

    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("TurtleBot"));
        thing_manager.AddThing(iot::CreateThing("Servos"));
        thing_manager.AddThing(iot::CreateThing("Display"));
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        // Arduino-style voice command mapping
        mcp_server.AddTool("self.turtle.come", "让乌龟机器人过来", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "Arduino Command: Come -> MoveForward(90, 6)");
                MoveForward(90, 6);
                return true;
            });

        mcp_server.AddTool("self.turtle.go", "让乌龟机器人去", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "Arduino Command: Go -> smoothMoveForward(6)");
                smoothMoveForward(6);
                return true;
            });

        mcp_server.AddTool("self.turtle.hand", "让乌龟机器人举手", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "Arduino Command: Hand -> servoLeftFront(60, 1, 1)");
                servoLeftFront(60, 1, 1);
                return true;
            });

        mcp_server.AddTool("self.turtle.dance", "让乌龟机器人跳舞", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(TAG, "Arduino Command: Dance -> MovementDance()");
                MovementDance();
                return true;
            });

        // Additional movement commands
        mcp_server.AddTool("self.turtle.move_forward", "向前移动", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                MoveForward(100, 4);
                return true;
            });

        mcp_server.AddTool("self.turtle.reset_position", "重置位置", PropertyList(), 
            [this](const PropertyList& properties) -> ReturnValue {
                MoveReset();
                return true;
            });
        mcp_server.AddTool("self.turtle.raise_hand", "Raise turtle robot hand", PropertyList(), 
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Voice Command: 'raise hand' -> servoLeftFront(60, 1, 1)");
            servoLeftFront(60, 1, 1);
            return true;
        });
        mcp_server.AddTool("self.turtle.wave", "Make turtle robot wave", PropertyList(), 
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Voice Command: 'wave' -> Wave sequence");
            // Wave sequence
            servoLeftFront(45, 5, 1);
            servoLeftFront(-45, 5, 1);
            servoLeftFront(45, 5, 1);
            servoLeftFront(-45, 5, 1);
            servoLeftFront(0, 5, 1);
            return true;
        });
        mcp_server.AddTool("self.turtle.look_left", "Make turtle robot look left", PropertyList(), 
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Voice Command: 'look left' -> servoHead(-30, 5, 2)");
            servoHead(-30, 5, 2);
            return true;
        });
        mcp_server.AddTool("self.turtle.look_right", "Make turtle robot look right", PropertyList(), 
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Voice Command: 'look right' -> servoHead(30, 5, 2)");
            servoHead(30, 5, 2);
            return true;
        });
        mcp_server.AddTool("self.turtle.hello", "Make turtle robot say hello", PropertyList(), 
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Voice Command: 'hello' -> Hello sequence");
            // Hello wave sequence
            servoLeftFront(50, 3, 1);
            servoHead(15, 3, 1);
            for (int i = 0; i < 3; i++) {
                servoLeftFront(30, 2, 1);
                servoLeftFront(50, 2, 1);
            }
            servoLeftFront(0, 5, 1);
            servoHead(0, 5, 1);
            return true;
        });
        mcp_server.AddTool("self.turtle.stretch", "Make turtle robot stretch", PropertyList(), 
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Voice Command: 'stretch' -> Stretch sequence");
            // Stretch all legs
            servoLeftFront(25, 10, 2);
            servoLeftBack(25, 10, 0);
            servoRightFront(25, 10, 0);
            servoRightBack(25, 10, 2);
            
            // Return to center
            MoveReset();
            return true;
        });
    }

public:
    // Constructor
    TurtleBot() {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeServos();
        InitializeIot();
        
        // Arduino-style initialization
        MoveInit();
        
        InitializeTools();
        
        ESP_LOGI(TAG, "MangDang Turtle Bot board initialized with Arduino-style API");
        
        if (display_ && display_ != nullptr) {
            display_->SetChatMessage("system", "TurtleBot Ready!");
            display_->SetEmotion("happy");
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN
        );
#else
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN
        );
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }
    virtual Backlight* GetBacklight() override { return nullptr; }

    // COMPLETE ARDUINO-STYLE ACTION.H API IMPLEMENTATION

    // Arduino MoveInit function
    void MoveInit() {
        ESP_LOGI(TAG, "Arduino MoveInit start");
        
        // Initialize AngS array with offset values
        for (int i = 0; i < 5; i++) {
            AngS_[i] = osang_[i];
        }
        
        ESP_LOGI(TAG, "Arduino MoveInit end");
    }

    // Arduino MoveReset function
    void MoveReset() {
        ESP_LOGI(TAG, "Arduino MoveReset start");
        if (display_) {
            display_->ShowNotification("Reset Position");
        }

        servoLeftFront(0, TIMES_WALK, SERVO_LOOP_DELAY);
        servoRightBack(0, TIMES_WALK, SERVO_LOOP_DELAY);
        servoRightFront(0, TIMES_WALK, SERVO_LOOP_DELAY);
        servoLeftBack(0, TIMES_WALK, SERVO_LOOP_DELAY);
        servoHead(0, TIMES_WALK, SERVO_LOOP_DELAY);

        ESP_LOGI(TAG, "Arduino MoveReset end");
    }

    // Arduino individual servo functions
    void servoLeftFront(int ange, int timewalk, int servo_delay) {
        if (timewalk < 1) {
            ESP_LOGE(TAG, "servo timewalk error!");
            return;
        }
        ESP_LOGI(TAG, "servoLeftFront start: ang = %d", ange);
        
        int angtmp = ange - AngS_[0];
        for (int i = 1; i <= timewalk; i++) {
            servo_write(0, input_ang(0, AngS_[0] + angtmp * i / timewalk));
            delay(servo_delay);
        }
        AngS_[0] = ange;
        ESP_LOGI(TAG, "servoLeftFront end!");
    }

    void servoLeftBack(int ange, int timewalk, int servo_delay) {
        if (timewalk < 1) {
            ESP_LOGE(TAG, "servo timewalk error!");
            return;
        }
        ESP_LOGI(TAG, "servoLeftBack start: ang = %d", ange);
        
        int angtmp = ange - AngS_[1];
        for (int i = 1; i <= timewalk; i++) {
            servo_write(1, input_ang(1, AngS_[1] + angtmp * i / timewalk));
            delay(servo_delay);
        }
        AngS_[1] = ange;
        ESP_LOGI(TAG, "servoLeftBack end!");
    }

    void servoRightFront(int ange, int timewalk, int servo_delay) {
        if (timewalk < 1) {
            ESP_LOGE(TAG, "servo timewalk error!");
            return;
        }
        ESP_LOGI(TAG, "servoRightFront start: ang = %d", ange);
        
        int angtmp = ange - AngS_[2];
        for (int i = 1; i <= timewalk; i++) {
            servo_write(2, input_ang(2, AngS_[2] + angtmp * i / timewalk));
            delay(servo_delay);
        }
        AngS_[2] = ange;
        ESP_LOGI(TAG, "servoRightFront end!");
    }

    void servoRightBack(int ange, int timewalk, int servo_delay) {
        if (timewalk < 1) {
            ESP_LOGE(TAG, "servo timewalk error!");
            return;
        }
        ESP_LOGI(TAG, "servoRightBack start: ang = %d", ange);
        
        int angtmp = ange - AngS_[3];
        for (int i = 1; i <= timewalk; i++) {
            servo_write(3, input_ang(3, AngS_[3] + angtmp * i / timewalk));
            delay(servo_delay);
        }
        AngS_[3] = ange;
        ESP_LOGI(TAG, "servoRightBack end!");
    }

    void servoHead(int ange, int timewalk, int servo_delay) {
        if (timewalk < 1) {
            ESP_LOGE(TAG, "servo timewalk error!");
            return;
        }
        ESP_LOGI(TAG, "servoHead start: ang = %d", ange);
        
        int angtmp = ange - AngS_[4];
        for (int i = 1; i <= timewalk; i++) {
            servo_write(4, input_ang(4, AngS_[4] + angtmp * i / timewalk));
            delay(servo_delay);
        }
        AngS_[4] = ange;
        ESP_LOGI(TAG, "servoHead end!");
    }

    // Arduino MoveForward function - EXACT IMPLEMENTATION
    void MoveForward(int step_delay, int loop_num) {
        ESP_LOGI(TAG, "Arduino MoveForward: step_delay=%d, loop_num=%d", step_delay, loop_num);
        if (display_) {
            display_->ShowNotification("Moving Forward");
        }

        for (int i = 0; i < loop_num; i++) {
            ESP_LOGI(TAG, "MoveForward loop count: %d", i);
            
            servoLeftFront(15, TIMES_WALK, SERVO_LOOP_DELAY);
            servoRightBack(15, TIMES_WALK, SERVO_LOOP_DELAY);
            servoRightFront(-15, TIMES_WALK, SERVO_LOOP_DELAY);
            servoLeftBack(-15, TIMES_WALK, SERVO_LOOP_DELAY);
            servoHead(15, TIMES_WALK, SERVO_LOOP_DELAY);
            
            delay(step_delay);
            
            servoLeftFront(0, TIMES_WALK, SERVO_LOOP_DELAY);
            servoRightBack(0, TIMES_WALK, SERVO_LOOP_DELAY);
            servoRightFront(0, TIMES_WALK, SERVO_LOOP_DELAY);
            servoLeftBack(0, TIMES_WALK, SERVO_LOOP_DELAY);
            servoHead(0, TIMES_WALK, SERVO_LOOP_DELAY);
            
            delay(step_delay);
        }
    }

    // Arduino smoothMoveForward function - EXACT IMPLEMENTATION
    void smoothMoveForward(int loopNum) {
        ESP_LOGI(TAG, "Arduino smoothMoveForward: loopNum=%d", loopNum);
        if (display_) {
            display_->ShowNotification("Smooth Forward");
        }

        // Arduino interpolation arrays - EXACT VALUES
        unsigned char angle1_position_init[INTERPOLATION_NUM1] = {90, 90, 90, 90, 90, 90, 90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90};
        unsigned char angle2_position_init[INTERPOLATION_NUM1] = {90, 90, 91, 92, 93, 94, 94,  95,  96,  97,  98,  98,  99,  100, 101, 102, 102, 103, 104, 105, 106, 106, 107, 108, 109, 110, 111, 111, 112, 113, 114};
        unsigned char angle3_position_init[INTERPOLATION_NUM1] = {90, 91, 93, 95, 96, 98, 100, 102, 103, 105, 107, 109, 110, 112, 114, 115, 117, 119, 121, 122, 124, 126, 128, 129, 131, 133, 135, 136, 138, 140, 141};
        unsigned char angle4_position_init[INTERPOLATION_NUM1] = {90, 87, 85, 83, 81, 79, 77,  75,  73,  71,  69,  67,  65,  63,  60,  58,  56,  54,  52,  50,  48,  46,  44,  42,  40,  38,  36,  33,  31,  29,  27};
        
        unsigned char angles1_tem[INTERPOLATION_NUM2] = {90, 88, 86, 84, 82, 80, 78, 76, 74, 72, 70, 68, 66, 64, 62, 62, 62, 62, 62, 62, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 48, 49, 51, 52, 54, 55, 56, 58, 59, 60, 62, 63, 65, 66, 67, 69, 70, 72, 73, 74, 76, 77, 78, 80, 81, 83, 84, 85, 87, 88, 90};
        unsigned char angles2_tem[INTERPOLATION_NUM2] = {114, 115, 115, 116, 117, 118, 119, 119, 120, 121, 122, 123, 123, 124, 125, 126, 127, 127, 128, 129, 130, 131, 132, 132, 133, 134, 135, 136, 136, 137, 138, 138, 137, 137, 136, 136, 135, 135, 135, 134, 134, 133, 133, 132, 132, 131, 131, 131, 131, 131, 131, 131, 130, 129, 127, 126, 125, 124, 122, 121, 120, 119, 117, 116, 115, 114};
        unsigned char angles3_tem[INTERPOLATION_NUM2] = {141, 140, 138, 136, 135, 133, 131, 129, 128, 126, 124, 122, 121, 119, 117, 115, 114, 112, 110, 109, 107, 105, 103, 102, 100, 98, 96, 95, 93, 91, 90, 90, 91, 93, 95, 97, 99, 101, 103, 105, 107, 109, 111, 113, 115, 117, 117, 117, 117, 117, 117, 117, 119, 121, 122, 124, 126, 128, 129, 131, 133, 135, 136, 138, 140, 141};
        unsigned char angles4_tem[INTERPOLATION_NUM2] = {27, 29, 30, 32, 33, 35, 36, 38, 39, 41, 42, 44, 45, 46, 48, 48, 48, 48, 48, 48, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 62, 61, 60, 58, 57, 56, 55, 54, 53, 51, 50, 49, 48, 47, 46, 45, 43, 42, 41, 40, 39, 38, 36, 35, 34, 33, 32, 31, 30, 28, 27};
        
        // From calibration position to walk position
        for (int j = 0; j < INTERPOLATION_NUM1; j++){
            servo_write(0, int(angle1_position_init[j]) - osang_[0]);
            servo_write(1, int(angle2_position_init[j]) - osang_[1]);
            servo_write(2, int(angle3_position_init[j]) + osang_[2]);
            servo_write(3, int(angle4_position_init[j]) + osang_[3]);
            delay(STEP_DELAY);
        }
        
        // Forward walk loop
        for (int i = 0; i <= loopNum; i++){
            for (int j = 0; j < INTERPOLATION_NUM2 ; j++){
                servo_write(0, int(angles1_tem[j]) - osang_[0]);
                servo_write(1, int(angles2_tem[j]) - osang_[1]);
                servo_write(2, int(angles3_tem[j]) + osang_[2]);
                servo_write(3, int(angles4_tem[j]) + osang_[3]);
                delay(STEP_DELAY);
            }
        }
        
        // From walk position to calibration position
        for (int j = 0; j < INTERPOLATION_NUM1; j++){
            servo_write(0, int(angle1_position_init[INTERPOLATION_NUM1 - 1 - j]) - osang_[0]);
            servo_write(1, int(angle2_position_init[INTERPOLATION_NUM1 - 1 - j]) - osang_[1]);
            servo_write(2, int(angle3_position_init[INTERPOLATION_NUM1 - 1 - j]) + osang_[2]);
            servo_write(3, int(angle4_position_init[INTERPOLATION_NUM1 - 1 - j]) + osang_[3]);
            delay(STEP_DELAY);
        }
    }

    // Arduino MovementDance function - EXACT IMPLEMENTATION
    void MovementDance() {
        ESP_LOGI(TAG, "Arduino MovementDance");
        if (display_) {
            display_->ShowNotification("Dancing!");
            display_->SetEmotion("happy");
        }

        int timewalk1 = 8;
        int timewalk2 = 16;
        int loop_times = 6;
        int stepDelay = 30;

        // Arduino dance arrays - EXACT VALUES
        unsigned char LF_angle_slow[] = {110, 109, 108, 107, 105, 104, 102, 100,  98,  95,  92,  89,  85,  81,76,  70,  63,  57,  51,  47,  43,  39,  37,  34,  32,  31,  29,  28,27,  27,  26,  26,  25};
        unsigned char LB_angle_slow[] = {160, 159, 157, 155, 153, 150, 148, 145, 142, 138, 134, 130, 126, 120,115, 109, 102,  96,  91,  87,  83,  81,  78,  77,  75,  74,  73,  72,71,  71,  70,  70,  70};
        unsigned char RF_angle_slow[] = {69,  70,  71,  72,  74,  75,  77,  79,  81,  84,  87,  90,  94,  98, 103, 109, 116, 122, 128, 132, 136, 140, 142, 145, 147, 148, 150, 151,152, 152, 153, 153, 154};
        unsigned char RB_angle_slow[] = {19,  20,  22,  24,  26,  29,  31,  34,  37,  41,  45,  49,  53,  59, 64,  70,  77,  83,  88,  92,  96,  98, 101, 102, 104, 105, 106, 107,108, 108, 109, 109, 109};

        unsigned char LF_angle_fast[] = {110, 109, 108, 105, 100,  93,  81,  61,  25};
        unsigned char LB_angle_fast[] = {160, 159, 157, 152, 145, 135, 121, 100,  70};
        unsigned char RF_angle_fast[] = {69,  70,  71,  74,  79,  86,  98,  118, 154};
        unsigned char RB_angle_fast[] = {19,  20,  22,  27,  34,  44,  58,  79,  109,};

        for (int i = 0; i < loop_times; i++) {
            for (int j = 0; j <= timewalk1; j++){
                servo_write(0, int(LF_angle_fast[j]) - osang_[0]);
                servo_write(1, int(LB_angle_fast[j]) - osang_[1]);
                servo_write(2, int(RF_angle_fast[j]) + osang_[2]);
                servo_write(3, int(RB_angle_fast[j]) + osang_[3]);
                delay(stepDelay);
            }

            for (int j = 0; j <= 2 * timewalk2; j++){
                servo_write(0, int(LF_angle_slow[2 * timewalk2 - j]) - osang_[0]);
                servo_write(1, int(LB_angle_slow[2 * timewalk2 - j]) - osang_[1]);
                servo_write(2, int(RF_angle_slow[2 * timewalk2 - j]) + osang_[2]);
                servo_write(3, int(RB_angle_slow[2 * timewalk2 - j]) + osang_[3]);
                delay(stepDelay);
            }
        }

        if (display_) {
            display_->SetEmotion("neutral");
        }
        ESP_LOGI(TAG, "Arduino MovementDance complete");
    }

    // Arduino smoothAngle helper function
    void smoothAngle(float angle1, float angle2, int timewalk, float nonlinear_flag) {
        float delt[timewalk + 1];
        float angles1[timewalk + 1];
        float angles2[timewalk];
        float k;
        float temp[timewalk + 1];
        
        for (int i = 0; i <= timewalk; i++){
            delt[i] = - 1.0 + 1.0 / float(timewalk) * float(i);
        }
        
        // k is the parameter to control the nonlinear feature
        if (nonlinear_flag == 1.0){k = 2.0;} else{k = 0.0;}
        
        for (int i = 0; i <= timewalk; i++){
            temp[i] = (1.0 + delt[i]) * exp(k * delt[i]);
        }
        
        for (int i = 0; i <= timewalk; i++){
            angles1[i] = angle1 + temp[i] * (angle2 - angle1) / 2.0;
        }
        
        for (int i = 0; i <= timewalk - 1; i++){
            angles2[i] = angle1 + angle2 - angles1[timewalk - 1 - i];
        }
        
        for (int i = 0; i <= 2 * timewalk ; i++){
            if (i <= timewalk) {
                angles3_[i] = angles1[i];
            } else {
                angles3_[i] = angles2[i-timewalk-1];
            }
        }
    }
};

// Register the turtle bot board
DECLARE_BOARD(TurtleBot);