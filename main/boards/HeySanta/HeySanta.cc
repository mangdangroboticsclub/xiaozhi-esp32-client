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
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "emoji_display.h"
// Add these new includes for web server
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/semphr.h"

#define TAG "HeySanta"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

float m1_coefficient = 1.0;
float m2_coefficient = 1.0;

// Global variables for web control
static httpd_handle_t motor_control_server = NULL;
static bool web_control_active = false;
static int web_controlled_head_speed = 0;
static SemaphoreHandle_t web_control_mutex = NULL;
static TaskHandle_t web_movement_task_handle = NULL;
static volatile bool web_movement_running = false;

class HeySantaCodec : public SantaAudioCodec  {
private:    

public:
    HeySantaCodec(i2c_master_bus_handle_t i2c_bus, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din, uint8_t es7210_addr, bool input_reference)
        : SantaAudioCodec(i2c_bus, input_sample_rate, output_sample_rate,
                             mclk,  bclk,  ws,  dout,  din, es7210_addr, input_reference) {}

    virtual void EnableOutput(bool enable) override {
        SantaAudioCodec::EnableOutput(enable);
    }
};

class HeySantaBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Button wake_button_;
    anim::EmojiWidget* display_ = nullptr;
    Esp32Camera* camera_;
    
    void Initialize_Motor(void)
    {
        // Prepare and then apply the LEDC PWM timer configuration
        ledc_timer_config_t ledc_timer = {
            .speed_mode       = LEDC_MODE,
            .duty_resolution  = LEDC_DUTY_RES,
            .timer_num        = LEDC_TIMER,
            .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
            .clk_cfg          = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

        // Array of channel configurations for easy iteration
        const uint8_t motor_ledc_channel[LEDC_CHANNEL_COUNT] = {LEDC_M1_CHANNEL_A, LEDC_M1_CHANNEL_B, LEDC_M2_CHANNEL_A, LEDC_M2_CHANNEL_B};
        const int32_t ledc_channel_pins[LEDC_CHANNEL_COUNT] = {LEDC_M1_CHANNEL_A_IO, LEDC_M1_CHANNEL_B_IO, LEDC_M2_CHANNEL_A_IO, LEDC_M2_CHANNEL_B_IO};
        for (int i = 0; i < LEDC_CHANNEL_COUNT; i++) {
            ledc_channel_config_t ledc_channel = {
                .gpio_num       = ledc_channel_pins[i],
                .speed_mode     = LEDC_MODE,
                .channel        = (ledc_channel_t)motor_ledc_channel[i],
                .intr_type      = LEDC_INTR_DISABLE,
                .timer_sel      = LEDC_TIMER,
                .duty           = 0, // Set duty to 0%
                .hpoint         = 0
            };
            ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        }
        
        // Initialize web control mutex
        web_control_mutex = xSemaphoreCreateMutex();
        if (web_control_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create web control mutex!");
        }
    }

    static void set_motor_A_speed(int speed)
    {
        if (speed >= 0) {
            uint32_t m1a_duty = (uint32_t)((speed * m1_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_A, m1a_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_B, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_B));
        } else {
            uint32_t m1b_duty = (uint32_t)((-speed * m1_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_A, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M1_CHANNEL_B, m1b_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M1_CHANNEL_B));
        }
    }

    static void set_motor_B_speed(int speed)
    {
        if (speed >= 0) {
            uint32_t m2a_duty = (uint32_t)((speed * m2_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_A, m2a_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_B, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_B));
        } else {
            uint32_t m2b_duty = (uint32_t)((-speed * m2_coefficient * 8192) / 100);
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_A, 0));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_A));
            ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_M2_CHANNEL_B, m2b_duty));
            ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_M2_CHANNEL_B));
        }
    }

    void SetHeadSpeed(int speed) {
        ESP_LOGI(TAG, "Setting head speed to %d", speed);
        set_motor_A_speed(speed);
    }

    void SetHipSpeed(int speed) {
        ESP_LOGI(TAG, "Setting hip speed to %d", speed);
        set_motor_B_speed(speed);    
    }

    // Web control motor functions
    static void set_web_head_speed(int speed) {
        web_controlled_head_speed = speed;
        set_motor_A_speed(speed);
    }

    static bool wait_with_stop_check(void) {
        for (int i = 0; i < 50; i++) { // Check every 10ms for 500ms total
            if (xSemaphoreTake(web_control_mutex, 0) == pdTRUE) {
                if (!web_movement_running) {
                    xSemaphoreGive(web_control_mutex);
                    return false;
                }
                xSemaphoreGive(web_control_mutex);
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        return true;
    }

    static void web_head_movement_task(void *pvParameters) {
        ESP_LOGI(TAG, "Starting web-controlled head movement task!");
        
        int speed_pattern[] = {30, 50, 70, 100, 70, 50, 30, -30, -50, -70, -100, -70, -50, -30};
        int pattern_length = sizeof(speed_pattern) / sizeof(speed_pattern[0]);
        int current_step = 0;
        
        set_web_head_speed(speed_pattern[0]);
        ESP_LOGI(TAG, "Head motor started at %d%% speed", speed_pattern[0]);
        
        while (1) {
            if (!wait_with_stop_check()) {
                break;
            }
            
            current_step = (current_step + 1) % pattern_length;
            int new_speed = speed_pattern[current_step];
            
            set_web_head_speed(new_speed);
            ESP_LOGI(TAG, "Head speed changed to %d%% (step %d/%d)", 
                     new_speed, current_step + 1, pattern_length);
        }
        
        set_web_head_speed(0);
        ESP_LOGI(TAG, "Web head movement task STOPPED!");
        
        if (xSemaphoreTake(web_control_mutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
            web_movement_task_handle = NULL;
            xSemaphoreGive(web_control_mutex);
        }
        vTaskDelete(NULL);
    }

    static void start_web_head_movement(void) {
        if (xSemaphoreTake(web_control_mutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
            if (web_movement_running) {
                ESP_LOGI(TAG, "Web movement already running, ignoring command");
                xSemaphoreGive(web_control_mutex);
                return;
            }
            
            web_movement_running = true;
            xTaskCreate(web_head_movement_task, "web_head_movement", 3072, NULL, 5, &web_movement_task_handle);
            ESP_LOGI(TAG, "Started web-controlled head movement task!");
            xSemaphoreGive(web_control_mutex);
        }
    }

    static void stop_web_head_movement(void) {
        ESP_LOGI(TAG, "Stopping web head movement...");
        
        if (xSemaphoreTake(web_control_mutex, 100 / portTICK_PERIOD_MS) == pdTRUE) {
            web_movement_running = false;
            xSemaphoreGive(web_control_mutex);
        }
        
        int wait_count = 0;
        while (web_movement_task_handle != NULL && wait_count < 30) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            wait_count++;
        }
        
        set_web_head_speed(0);
        
        if (web_movement_task_handle != NULL) {
            ESP_LOGW(TAG, "Force deleting stuck web movement task!");
            vTaskDelete(web_movement_task_handle);
            web_movement_task_handle = NULL;
        }
        
        ESP_LOGI(TAG, "All web movement STOPPED!");
    }

    // Web server handlers
    static esp_err_t motor_control_page_handler(httpd_req_t *req) {
        const char* html = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<title>🎅 HeySanta Body Shake Control</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body { font-family: 'Arial', sans-serif; text-align: center; margin: 0; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; min-height: 100vh; }"
        ".container { max-width: 500px; margin: 0 auto; background: rgba(255,255,255,0.15); padding: 30px; border-radius: 20px; backdrop-filter: blur(10px); box-shadow: 0 8px 32px rgba(0,0,0,0.3); border: 1px solid rgba(255,255,255,0.2); }"
        "h1 { color: #fff; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.5); font-size: 28px; }"
        "button { padding: 15px 30px; margin: 10px; font-size: 18px; border: none; border-radius: 15px; cursor: pointer; min-width: 140px; transition: all 0.3s ease; font-weight: bold; }"
        ".start { background: linear-gradient(45deg, #4CAF50, #45a049); color: white; box-shadow: 0 4px 15px rgba(76, 175, 80, 0.4); }"
        ".stop { background: linear-gradient(45deg, #f44336, #d32f2f); color: white; box-shadow: 0 4px 15px rgba(244, 67, 54, 0.4); }"
        ".status-btn { background: linear-gradient(45deg, #FF9800, #F57C00); color: white; box-shadow: 0 4px 15px rgba(255, 152, 0, 0.4); }"
        "button:hover { transform: translateY(-2px); box-shadow: 0 6px 20px rgba(0,0,0,0.3); }"
        "button:active { transform: translateY(0); }"
        ".speed-control { margin: 30px 0; padding: 25px; background: rgba(255,255,255,0.1); border-radius: 15px; backdrop-filter: blur(5px); border: 1px solid rgba(255,255,255,0.2); }"
        ".slider-container { margin: 20px 0; }"
        ".speed-slider {"
        "  -webkit-appearance: none;"
        "  width: 100%;"
        "  height: 8px;"
        "  border-radius: 15px;"
        "  background: linear-gradient(to right, #ddd 0%, #ddd 100%);"
        "  outline: none;"
        "  opacity: 0.8;"
        "  transition: opacity 0.2s;"
        "  margin: 20px 0;"
        "}"
        ".speed-slider:hover { opacity: 1; }"
        ".speed-slider::-webkit-slider-thumb {"
        "  -webkit-appearance: none;"
        "  appearance: none;"
        "  width: 30px;"
        "  height: 30px;"
        "  border-radius: 50%;"
        "  background: linear-gradient(45deg, #2196F3, #1976D2);"
        "  cursor: pointer;"
        "  box-shadow: 0 2px 8px rgba(33, 150, 243, 0.5);"
        "  border: 3px solid white;"
        "}"
        ".speed-slider::-moz-range-thumb {"
        "  width: 30px;"
        "  height: 30px;"
        "  border-radius: 50%;"
        "  background: linear-gradient(45deg, #2196F3, #1976D2);"
        "  cursor: pointer;"
        "  border: 3px solid white;"
        "  box-shadow: 0 2px 8px rgba(33, 150, 243, 0.5);"
        "}"
        ".speed-display {"
        "  font-size: 32px;"
        "  font-weight: bold;"
        "  color: #fff;"
        "  margin: 15px 0;"
        "  padding: 15px;"
        "  background: linear-gradient(45deg, #2196F3, #1976D2);"
        "  border-radius: 15px;"
        "  display: inline-block;"
        "  min-width: 120px;"
        "  text-shadow: 1px 1px 2px rgba(0,0,0,0.3);"
        "  box-shadow: 0 4px 15px rgba(33, 150, 243, 0.4);"
        "}"
        ".speed-labels {"
        "  display: flex;"
        "  justify-content: space-between;"
        "  font-size: 14px;"
        "  color: rgba(255,255,255,0.8);"
        "  margin-top: 10px;"
        "  font-weight: bold;"
        "}"
        ".status { margin: 20px 0; padding: 15px; background: rgba(255,255,255,0.1); border-radius: 10px; font-weight: bold; backdrop-filter: blur(5px); border: 1px solid rgba(255,255,255,0.2); }"
        ".zero-btn { background: linear-gradient(45deg, #9E9E9E, #757575); color: white; margin: 10px 5px; padding: 12px 25px; font-size: 16px; box-shadow: 0 4px 15px rgba(158, 158, 158, 0.4); }"
        ".direction-control { margin: 20px 0; display: flex; justify-content: center; gap: 10px; flex-wrap: wrap; }"
        ".direction-btn { background: linear-gradient(45deg, #9C27B0, #7B1FA2); color: white; padding: 12px 20px; font-size: 16px; border-radius: 10px; min-width: 100px; }"
        "@media (max-width: 600px) {"
        "  .container { margin: 10px; padding: 20px; }"
        "  h1 { font-size: 24px; }"
        "  button { padding: 12px 20px; font-size: 16px; min-width: 120px; }"
        "  .speed-display { font-size: 28px; }"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<h1>🎅 HeySanta Body Shake Control</h1>"
        
        "<div>"
        "<button class='start' onclick=\"sendCommand('/start')\">🟢 START PATTERN</button>"
        "<button class='stop' onclick=\"sendCommand('/stop')\">🔴 STOP ALL</button>"
        "</div>"
        
        "<div class='speed-control'>"
        "<h3 style='color: white; margin-bottom: 20px;'>🎛️ Manual Head Speed Control</h3>"
        "<div class='speed-display' id='speedDisplay'>0%</div>"
        "<div class='slider-container'>"
        "<input type='range' min='-100' max='100' value='0' class='speed-slider' id='speedSlider' oninput='updateSpeed(this.value)' onchange='setSliderSpeed(this.value)'>"
        "<div class='speed-labels'>"
        "<span>-100%</span>"
        "<span>-50%</span>"
        "<span>0%</span>"
        "<span>50%</span>"
        "<span>100%</span>"
        "</div>"
        "</div>"
        "<div class='direction-control'>"
        "<button class='direction-btn' onclick=\"setDirectionalSpeed(-75)\">⬅️ Left 75%</button>"
        "<button class='zero-btn' onclick=\"setZeroSpeed()\">⏹️ STOP</button>"
        "<button class='direction-btn' onclick=\"setDirectionalSpeed(75)\">➡️ Right 75%</button>"
        "</div>"
        "</div>"
        
        "<div>"
        "<button class='status-btn' onclick=\"getStatus()\">📊 STATUS</button>"
        "</div>"
        
        "<div id='status' class='status'>🎅 Ready to shake!</div>"
        "</div>"
        
        "<script>"
        "let isSliderActive = false;"
        "let lastSliderUpdate = 0;"
        
        "function updateSpeed(value) {"
        "  const absValue = Math.abs(value);"
        "  const direction = value >= 0 ? 'RIGHT' : 'LEFT';"
        "  const display = value == 0 ? '0%' : `${direction} ${absValue}%`;"
        "  document.getElementById('speedDisplay').innerText = display;"
        "  updateSliderBackground(value);"
        "}"
        
        "function updateSliderBackground(value) {"
        "  const slider = document.getElementById('speedSlider');"
        "  const absValue = Math.abs(value);"
        "  const percentage = (absValue / 100) * 100;"
        "  if (value >= 0) {"
        "    slider.style.background = `linear-gradient(to right, #ddd 0%, #ddd 50%, #4CAF50 50%, #4CAF50 ${50 + percentage/2}%, #ddd ${50 + percentage/2}%, #ddd 100%)`;"
        "  } else {"
        "    slider.style.background = `linear-gradient(to right, #ddd 0%, #ddd ${50 - percentage/2}%, #f44336 ${50 - percentage/2}%, #f44336 50%, #ddd 50%, #ddd 100%)`;"
        "  }"
        "}"
        
        "function setSliderSpeed(value) {"
        "  const now = Date.now();"
        "  if (now - lastSliderUpdate < 100) return;"
        "  lastSliderUpdate = now;"
        "  "
        "  if (parseInt(value) === 0) {"
        "    sendCommand('/stop');"
        "  } else {"
        "    sendCommand('/speed?value=' + value);"
        "  }"
        "}"
        
        "function setDirectionalSpeed(speed) {"
        "  document.getElementById('speedSlider').value = speed;"
        "  updateSpeed(speed);"
        "  sendCommand('/speed?value=' + speed);"
        "}"
        
        "function setZeroSpeed() {"
        "  document.getElementById('speedSlider').value = 0;"
        "  updateSpeed(0);"
        "  sendCommand('/stop');"
        "}"
        
        "function sendCommand(url) {"
        "  fetch(url)"
        "    .then(response => response.text())"
        "    .then(data => {"
        "      document.getElementById('status').innerText = data;"
        "      document.getElementById('status').style.background = 'rgba(76, 175, 80, 0.3)';"
        "    })"
        "    .catch(error => {"
        "      document.getElementById('status').innerText = '❌ Error: ' + error;"
        "      document.getElementById('status').style.background = 'rgba(244, 67, 54, 0.3)';"
        "    });"
        "}"
        
        "function getStatus() {"
        "  sendCommand('/status');"
        "}"
        
        "// Initialize slider appearance"
        "updateSliderBackground(0);"
        
        "// Auto-refresh status every 3 seconds"
        "setInterval(getStatus, 3000);"
        
        "// Update slider position based on current motor speed"
        "function updateSliderFromStatus() {"
        "  fetch('/current_speed')"
        "    .then(response => response.text())"
        "    .then(speed => {"
        "      const slider = document.getElementById('speedSlider');"
        "      if (!isSliderActive) {"
        "        slider.value = speed;"
        "        updateSpeed(speed);"
        "      }"
        "    })"
        "    .catch(error => console.log('Speed sync error:', error));"
        "}"
        
        "// Track when user is actively using the slider"
        "document.getElementById('speedSlider').addEventListener('mousedown', () => isSliderActive = true);"
        "document.getElementById('speedSlider').addEventListener('mouseup', () => setTimeout(() => isSliderActive = false, 500));"
        "document.getElementById('speedSlider').addEventListener('touchstart', () => isSliderActive = true);"
        "document.getElementById('speedSlider').addEventListener('touchend', () => setTimeout(() => isSliderActive = false, 500));"
        
        "// Sync slider with motor speed every 2 seconds"
        "setInterval(updateSliderFromStatus, 2000);"
        "</script>"
        "</body>"
        "</html>";
        
        httpd_resp_send(req, html, strlen(html));
        return ESP_OK;
    }

    static esp_err_t start_handler(httpd_req_t *req) {
        start_web_head_movement();
        httpd_resp_send(req, "🟢 Head shake pattern started! (30% → 50% → 70% → 100% → reverse)", -1);
        return ESP_OK;
    }

    static esp_err_t stop_handler(httpd_req_t *req) {
        stop_web_head_movement();
        httpd_resp_send(req, "🔴 Head movement stopped", -1);
        return ESP_OK;
    }

    static esp_err_t speed_handler(httpd_req_t *req) {
        char query[64];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char value_str[16];
            if (httpd_query_key_value(query, "value", value_str, sizeof(value_str)) == ESP_OK) {
                int speed = atoi(value_str);
                if (speed >= -100 && speed <= 100) {
                    stop_web_head_movement(); // Stop pattern movement first
                    set_web_head_speed(speed);
                    char response[128];
                    if (speed == 0) {
                        snprintf(response, sizeof(response), "⚡ Head stopped (speed set to %d%%)", speed);
                    } else if (speed > 0) {
                        snprintf(response, sizeof(response), "⚡ Head moving RIGHT at %d%% (pattern stopped)", speed);
                    } else {
                        snprintf(response, sizeof(response), "⚡ Head moving LEFT at %d%% (pattern stopped)", abs(speed));
                    }
                    httpd_resp_send(req, response, strlen(response));
                    return ESP_OK;
                }
            }
        }
        httpd_resp_send(req, "❌ Invalid speed value (must be -100 to 100)", -1);
        return ESP_OK;
    }

    static esp_err_t current_speed_handler(httpd_req_t *req) {
        char response[16];
        snprintf(response, sizeof(response), "%d", web_controlled_head_speed);
        httpd_resp_send(req, response, strlen(response));
        return ESP_OK;
    }

    static esp_err_t status_handler(httpd_req_t *req) {
        char response[256];
        const char* movement_status = web_movement_running ? "🔄 RUNNING (Pattern Mode)" : "⏸️ STOPPED";
        const char* speed_info = "";
        
        if (!web_movement_running && web_controlled_head_speed != 0) {
            speed_info = " (Manual Mode)";
        }
        
        snprintf(response, sizeof(response), 
                 "📊 Head Status: %s%s | Speed: %d%% | 🎅 HeySanta Ready!", 
                 movement_status, speed_info, web_controlled_head_speed);
        
        httpd_resp_send(req, response, strlen(response));
        return ESP_OK;
    }

    void start_motor_control_webserver(void) {
        if (motor_control_server != NULL) {
            ESP_LOGI(TAG, "Motor control web server already running");
            return;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;  // Use different port to avoid conflicts
        config.lru_purge_enable = true;
        
        ESP_LOGI(TAG, "Starting motor control HTTP server on port: %d", config.server_port);
        if (httpd_start(&motor_control_server, &config) == ESP_OK) {
            // Set URI handlers
            httpd_uri_t uri_get = {
                .uri       = "/",
                .method    = HTTP_GET,
                .handler   = motor_control_page_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(motor_control_server, &uri_get);

            httpd_uri_t uri_start = {
                .uri       = "/start",
                .method    = HTTP_GET,
                .handler   = start_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(motor_control_server, &uri_start);

            httpd_uri_t uri_stop = {
                .uri       = "/stop",
                .method    = HTTP_GET,
                .handler   = stop_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(motor_control_server, &uri_stop);

            httpd_uri_t uri_speed = {
                .uri       = "/speed",
                .method    = HTTP_GET,
                .handler   = speed_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(motor_control_server, &uri_speed);

            httpd_uri_t uri_current_speed = {
                .uri       = "/current_speed",
                .method    = HTTP_GET,
                .handler   = current_speed_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(motor_control_server, &uri_current_speed);

            httpd_uri_t uri_status = {
                .uri       = "/status",
                .method    = HTTP_GET,
                .handler   = status_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(motor_control_server, &uri_status);

            ESP_LOGI(TAG, "🎅 Motor control web server started successfully!");
            web_control_active = true;
        } else {
            ESP_LOGE(TAG, "Failed to start motor control web server!");
        }
    }

    void stop_motor_control_webserver(void) {
        if (motor_control_server != NULL) {
            stop_web_head_movement();  // Stop any running movements
            httpd_stop(motor_control_server);
            motor_control_server = NULL;
            web_control_active = false;
            ESP_LOGI(TAG, "Motor control web server stopped");
        }
    }

    void SparkBotDance() {
        ESP_LOGI(TAG, "Starting simple on/off dance!");
        
        for (int cnt = 0; cnt < 5; cnt++) {
            // Head shake sequence - much faster
            for (int i = 0; i < 10; i++) {
                SetHeadSpeed(100);  // Full speed forward
                vTaskDelay(100 / portTICK_PERIOD_MS);
                SetHeadSpeed(-100); // Full speed backward
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            SetHeadSpeed(0);  // Stop head
            
            // Hip shake sequence - gentler with stops
            for (int i = 0; i < 8; i++) {
                SetHipSpeed(100);   // Forward
                vTaskDelay(150 / portTICK_PERIOD_MS);
                SetHipSpeed(0);     // Stop
                vTaskDelay(50 / portTICK_PERIOD_MS);
                SetHipSpeed(-100);  // Backward
                vTaskDelay(150 / portTICK_PERIOD_MS);
                SetHipSpeed(0);     // Stop
                vTaskDelay(50 / portTICK_PERIOD_MS);
            }
            SetHipSpeed(0);  // Stop hip
            
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "Dance complete!");
    }

    void HeadShakeOnly() {
        ESP_LOGI(TAG, "Head shake (on/off mode)!");
        
        for (int i = 0; i < 50; i++) {
            SetHeadSpeed(100);   // Full speed forward
            vTaskDelay(80 / portTICK_PERIOD_MS);
            
            SetHeadSpeed(-100);  // Full speed backward
            vTaskDelay(80 / portTICK_PERIOD_MS);
        }
        SetHeadSpeed(0);
        ESP_LOGI(TAG, "Head shake complete!");
    }

    void HeadShake_start() {
        ESP_LOGI(TAG, "start ");
        for (int i = 0; i < 2; i++) {
            SetHeadSpeed(100);   // Full speed forward
            vTaskDelay(80 / portTICK_PERIOD_MS);
            
            SetHeadSpeed(-100);  // Full speed backward
            vTaskDelay(80 / portTICK_PERIOD_MS);
        }
    }

    void HeadShake_stop() {
        ESP_LOGI(TAG, "stop Head shake (on/off mode)!");
        for (int i = 0; i < 1; i++) {
            SetHeadSpeed(100);   // Full speed forward
            vTaskDelay(80 / portTICK_PERIOD_MS);
            
            SetHeadSpeed(-100);  // Full speed backward
            vTaskDelay(80 / portTICK_PERIOD_MS);
        }
        SetHeadSpeed(0);
    }

    void HipShakeOnly() {
        ESP_LOGI(TAG, "Hip shake (on/off mode)!");
        
        for (int i = 0; i < 12; i++) {
            SetHipSpeed(100);    // Forward
            vTaskDelay(150 / portTICK_PERIOD_MS);
            SetHipSpeed(0);      // Stop - adds gentleness
            vTaskDelay(50 / portTICK_PERIOD_MS);
            SetHipSpeed(-100);   // Backward
            vTaskDelay(150 / portTICK_PERIOD_MS);
            SetHipSpeed(0);      // Stop - adds gentleness
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        SetHipSpeed(0);
        ESP_LOGI(TAG, "Hip shake complete!");
    }

    void PulsedHeadMovement(int cycles) {
        ESP_LOGI(TAG, "Pulsed head movement!");
        
        for (int i = 0; i < cycles; i++) {
            // Quick bursts
            SetHeadSpeed(100);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            SetHeadSpeed(0);
            vTaskDelay(30 / portTICK_PERIOD_MS);
            
            SetHeadSpeed(-100);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            SetHeadSpeed(0);
            vTaskDelay(30 / portTICK_PERIOD_MS);
        }
        SetHeadSpeed(0);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        mcp_server.AddTool("self.chassis.dance", "跳舞", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Dance command received");
            SparkBotDance();
            return true;
        });
        
        mcp_server.AddTool("self_chassis_shake_body", "摇头", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Head shake command received");
            HeadShakeOnly();
            return true;
        });
        
        // NEW MCP TOOL: Open motor control panel
        mcp_server.AddTool("open_motor_control_panel", "开启摇头控制面板", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "🎅 Opening motor control panel...");
            
            if (!WifiStation::GetInstance().IsConnected()) {
                ESP_LOGW(TAG, "WiFi not connected, cannot start web server");
                return "❌ WiFi not connected. Please connect to WiFi first.";
            }
            
            // Start the web server
            start_motor_control_webserver();
            
            // Get IP address
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(netif, &ip_info);
            
            char response[256];
            snprintf(response, sizeof(response), 
                    "🎅 HeySanta Motor Control Panel Started!\n"
                    "📱 Open your browser and go to:\n"
                    "🌐 http://" IPSTR ":8080\n"
                    "🎛️ Control head shake speed with precision!",
                    IP2STR(&ip_info.ip));
            
            ESP_LOGI(TAG, "%s", response);
            return response;
        });
        
        // Tool to close motor control panel
        mcp_server.AddTool("close_motor_control_panel", "关闭摇头控制面板", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "🎅 Closing motor control panel...");
            stop_motor_control_webserver();
            return "🔴 Motor control panel closed. Web server stopped.";
        });
        
        mcp_server.AddTool("self_chassis_shake_hip", "摇屁股", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Hip shake command received");
            HipShakeOnly();
            return true;
        });

        mcp_server.AddTool("self_chassis_pulse_head", "脉冲摇头", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Pulsed head movement command received");
            PulsedHeadMovement(20);
            return true;
        });

        mcp_server.AddTool("self_chassis_shake_body_start", "摇头1", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            HeadShake_start();
            return true;
        });

        mcp_server.AddTool("self_chassis_shake_body_stop", "摇头2", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            HeadShake_stop();
            return true;
        });
    }

    void InitializeI2c() {
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
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

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
        
        ESP_LOGI(TAG, "Panel handle: %p, Panel IO handle: %p", panel, panel_io);
        display_ = new anim::EmojiWidget(panel, panel_io);
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_3;
        config.ledc_timer = LEDC_TIMER_1;
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
        config.pin_sccb_sda = -1;
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

public:
    HeySantaBoard() : boot_button_(BOOT_BUTTON_GPIO), wake_button_(WAKE_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeCamera();
        Initialize_Motor();
        InitializeTools();

#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
#endif
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static HeySantaCodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
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

DECLARE_BOARD(HeySantaBoard);