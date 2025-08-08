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
// Add these includes for web server
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <string.h>
#include <cstdlib>


#define TAG "HeySanta"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

float m1_coefficient = 1.0;
float m2_coefficient = 1.0;

// Global variables for web server
static httpd_handle_t speech_server = NULL;
static bool web_speech_active = false;

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
    void movement_type(int motor, uint32_t mode, int dir) 
    //motor: 1 for head, 2 for hip 
    //mode: 0 for slow, 1 for mid, 2 for stop 
    //dir: 1 for forward, -1 for backward
    {
        int speed_head[3] = {87, 93, 100}; 
        int speed_hip[3] = {90, 95, 100};
        int speed;
        if (motor == 1)  
        {
            if (dir == 1) speed = speed_head[mode];
            else speed = -speed_head[mode];
            ESP_LOGI(TAG, "Setting head speed to %d", speed);
            set_motor_A_speed(speed);
        }
        else 
        {
            if (dir == 1) speed = speed_hip[mode];
            else speed = -speed_hip[mode];
            ESP_LOGI(TAG, "Setting hip speed to %d", speed);
            if (speed < 0) {
                speed = abs(speed);
                int step = speed / 4;
                for (int i = 1 ; i <= 4 ; i++)
                {
                    set_motor_B_speed(speed); // Gradually increase speed to avoid sudden jerk
                    speed -= step; 
                    vTaskDelay(25 / portTICK_PERIOD_MS); // Wait for 0.1 second    
                }
                set_motor_B_speed(0); // Set final speed
            }
            else 
            {
                set_motor_B_speed(speed);
            }
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

    // Web server handlers for speech only
    static esp_err_t speech_control_page_handler(httpd_req_t *req) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        
        const char* html = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<title>&#127876; Santa Speech Control</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body { font-family: 'Arial', sans-serif; text-align: center; margin: 0; padding: 20px; background: linear-gradient(135deg, #2E7D32 0%, #C62828 50%, #2E7D32 100%); color: white; min-height: 100vh; }"
        ".container { max-width: 600px; margin: 0 auto; background: rgba(255,255,255,0.15); padding: 40px; border-radius: 25px; backdrop-filter: blur(15px); box-shadow: 0 10px 40px rgba(0,0,0,0.3); border: 2px solid rgba(255,255,255,0.2); }"
        "h1 { color: #fff; margin-bottom: 30px; text-shadow: 3px 3px 6px rgba(0,0,0,0.5); font-size: 32px; animation: glow 2s ease-in-out infinite alternate; }"
        "@keyframes glow { from { text-shadow: 3px 3px 6px rgba(0,0,0,0.5), 0 0 10px rgba(255,255,255,0.3); } to { text-shadow: 3px 3px 6px rgba(0,0,0,0.5), 0 0 20px rgba(255,255,255,0.6); } }"
        "h2 { color: #fff; margin: 30px 0 20px 0; text-shadow: 2px 2px 4px rgba(0,0,0,0.5); font-size: 24px; }"
        "button { padding: 15px 30px; margin: 10px; font-size: 18px; border: none; border-radius: 15px; cursor: pointer; min-width: 160px; transition: all 0.3s ease; font-weight: bold; }"
        ".speak-btn { background: linear-gradient(45deg, #4CAF50, #45a049); color: white; box-shadow: 0 6px 20px rgba(76, 175, 80, 0.4); }"
        ".stop-btn { background: linear-gradient(45deg, #F44336, #D32F2F); color: white; box-shadow: 0 6px 20px rgba(244, 67, 54, 0.4); }"
        ".status-btn { background: linear-gradient(45deg, #FF9800, #F57C00); color: white; box-shadow: 0 6px 20px rgba(255, 152, 0, 0.4); }"
        ".clear-btn { background: linear-gradient(45deg, #9E9E9E, #757575); color: white; box-shadow: 0 6px 20px rgba(158, 158, 158, 0.4); }"
        "button:hover { transform: translateY(-3px); box-shadow: 0 8px 25px rgba(0,0,0,0.4); }"
        "button:active { transform: translateY(0); }"
        ".speech-control { margin: 30px 0; padding: 30px; background: rgba(255,255,255,0.1); border-radius: 20px; backdrop-filter: blur(10px); border: 2px solid rgba(255,255,255,0.2); }"
        ".speech-input {"
        "  width: 100%;"
        "  padding: 20px;"
        "  font-size: 18px;"
        "  border: 3px solid rgba(255,255,255,0.3);"
        "  border-radius: 15px;"
        "  background: rgba(255,255,255,0.1);"
        "  color: white;"
        "  backdrop-filter: blur(10px);"
        "  margin: 15px 0;"
        "  box-sizing: border-box;"
        "  transition: all 0.3s ease;"
        "}"
        ".speech-input::placeholder { color: rgba(255,255,255,0.7); }"
        ".speech-input:focus { outline: none; border-color: #4CAF50; background: rgba(255,255,255,0.2); box-shadow: 0 0 15px rgba(76, 175, 80, 0.5); }"
        ".preset-messages { display: flex; flex-wrap: wrap; gap: 12px; justify-content: center; margin: 20px 0; }"
        ".preset-btn {"
        "  background: linear-gradient(45deg, #E91E63, #C2185B);"
        "  color: white;"
        "  padding: 12px 18px;"
        "  font-size: 16px;"
        "  border-radius: 12px;"
        "  cursor: pointer;"
        "  border: none;"
        "  transition: all 0.3s ease;"
        "  box-shadow: 0 4px 15px rgba(233, 30, 99, 0.4);"
        "  min-width: 140px;"
        "}"
        ".preset-btn:hover { transform: translateY(-2px); box-shadow: 0 6px 20px rgba(233, 30, 99, 0.6); }"
        ".status { margin: 25px 0; padding: 20px; background: rgba(255,255,255,0.1); border-radius: 15px; font-weight: bold; backdrop-filter: blur(10px); border: 2px solid rgba(255,255,255,0.2); font-size: 18px; }"
        ".santa-emoji { font-size: 48px; margin: 20px 0; animation: bounce 2s infinite; }"
        "@keyframes bounce { 0%, 20%, 50%, 80%, 100% { transform: translateY(0); } 40% { transform: translateY(-10px); } 60% { transform: translateY(-5px); } }"
        ".char-counter { font-size: 14px; color: rgba(255,255,255,0.8); margin-top: 5px; }"
        ".control-buttons { display: flex; flex-wrap: wrap; gap: 15px; justify-content: center; margin: 20px 0; }"
        "@media (max-width: 600px) {"
        "  .container { margin: 10px; padding: 25px; }"
        "  h1 { font-size: 28px; }"
        "  .speech-input { font-size: 16px; padding: 15px; }"
        "  button { padding: 12px 20px; font-size: 16px; min-width: 130px; }"
        "  .preset-btn { min-width: 120px; font-size: 14px; }"
        "  .control-buttons { flex-direction: column; }"
        "}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<div class='santa-emoji'>&#127876;</div>"
        "<h1>&#127876; Santa Speech Control &#127876;</h1>"
        
        "<div class='speech-control'>"
        "<h2>&#128483; Make Santa Speak</h2>"
        "<input type='text' class='speech-input' id='speechText' placeholder='Type what Santa should say... Ho ho ho!' maxlength='300'>"
        "<div class='char-counter'><span id='charCount'>0</span>/300 characters</div>"
        
        "<div class='preset-messages'>"
        "<button class='preset-btn' onclick=\"setSpeechText('Ho ho ho! Merry Christmas everyone!')\">&#127876; Merry Christmas</button>"
        "<button class='preset-btn' onclick=\"setSpeechText('Have you been good this year?')\">&#128519; Been Good?</button>"
        "<button class='preset-btn' onclick=\"setSpeechText('What would you like for Christmas?')\">&#127873; Christmas Wish</button>"
        "<button class='preset-btn' onclick=\"setSpeechText('Time for milk and cookies!')\">&#127850; Milk & Cookies</button>"
        "<button class='preset-btn' onclick=\"setSpeechText('Ho ho ho! I can see you!')\">&#128064; I See You</button>"
        "<button class='preset-btn' onclick=\"setSpeechText('Christmas magic is in the air!')\">&#10024; Christmas Magic</button>"
        "</div>"
        
        "<div class='control-buttons'>"
        "<button class='speak-btn' onclick='makeSantaSpeak()'>&#127876; MAKE SANTA SPEAK</button>"
        "<button class='stop-btn' onclick='stopSanta()'>&#9209; STOP SANTA</button>"
        "<button class='clear-btn' onclick='clearText()'>&#128465; CLEAR</button>"
        "</div>"
        "</div>"
        
        "<div>"
        "<button class='status-btn' onclick='getStatus()'>&#128202; CHECK STATUS</button>"
        "</div>"
        
        "<div id='status' class='status'>&#127876; Ho ho ho! Ready to spread Christmas joy!</div>"
        "</div>"
        
        "<script>"
        "console.log('Santa page loading...');"
        "const speechInput = document.getElementById('speechText');"
        "const charCount = document.getElementById('charCount');"
        
        "speechInput.addEventListener('input', function() {"
        "  charCount.textContent = this.value.length;"
        "  if (this.value.length > 250) {"
        "    charCount.style.color = '#ff6b6b';"
        "  } else {"
        "    charCount.style.color = 'rgba(255,255,255,0.8)';"
        "  }"
        "});"
        
        "function setSpeechText(text) {"
        "  console.log('Setting speech text:', text);"
        "  speechInput.value = text;"
        "  speechInput.dispatchEvent(new Event('input'));"
        "}"
        
        "function clearText() {"
        "  console.log('Clearing text');"
        "  speechInput.value = '';"
        "  speechInput.dispatchEvent(new Event('input'));"
        "  speechInput.focus();"
        "}"
        
        "function makeSantaSpeak() {"
        "  console.log('MAKE SANTA SPEAK button clicked!');"
        "  const text = speechInput.value.trim();"
        "  console.log('Text to speak:', text);"
        "  "
        "  if (text === '') {"
        "    console.warn('No text provided');"
        "    document.getElementById('status').innerText = 'Please enter some text for Santa to say!';"
        "    document.getElementById('status').style.background = 'rgba(244, 67, 54, 0.3)';"
        "    speechInput.focus();"
        "    return;"
        "  }"
        "  "
        "  console.log('Sending fetch request to /speak');"
        "  document.getElementById('status').innerText = 'Santa is preparing to speak...';"
        "  document.getElementById('status').style.background = 'rgba(255, 193, 7, 0.3)';"
        "  "
        "  const url = '/speak?text=' + encodeURIComponent(text);"
        "  console.log('Full URL:', url);"
        "  "
        "  fetch(url)"
        "    .then(response => {"
        "      console.log('Response received:', response.status, response.statusText);"
        "      return response.text();"
        "    })"
        "    .then(data => {"
        "      console.log('Response data:', data);"
        "      document.getElementById('status').innerText = data;"
        "      document.getElementById('status').style.background = 'rgba(76, 175, 80, 0.3)';"
        "      speechInput.value = '';"
        "      speechInput.dispatchEvent(new Event('input'));"
        "    })"
        "    .catch(error => {"
        "      console.error('Fetch error:', error);"
        "      document.getElementById('status').innerText = 'Speech Error: ' + error;"
        "      document.getElementById('status').style.background = 'rgba(244, 67, 54, 0.3)';"
        "    });"
        "}"
        
        // NEW STOP FUNCTION
        "function stopSanta() {"
        "  console.log('STOP SANTA button clicked!');"
        "  document.getElementById('status').innerText = 'Stopping Santa...';"
        "  document.getElementById('status').style.background = 'rgba(244, 67, 54, 0.3)';"
        "  "
        "  fetch('/stop')"
        "    .then(response => {"
        "      console.log('Stop response:', response.status);"
        "      return response.text();"
        "    })"
        "    .then(data => {"
        "      console.log('Stop response data:', data);"
        "      document.getElementById('status').innerText = data;"
        "      document.getElementById('status').style.background = 'rgba(255, 152, 0, 0.3)';"
        "    })"
        "    .catch(error => {"
        "      console.error('Stop error:', error);"
        "      document.getElementById('status').innerText = 'Stop Error: ' + error;"
        "      document.getElementById('status').style.background = 'rgba(244, 67, 54, 0.3)';"
        "    });"
        "}"
        
        "function getStatus() {"
        "  console.log('Getting status...');"
        "  fetch('/status')"
        "    .then(response => {"
        "      console.log('Status response:', response.status);"
        "      return response.text();"
        "    })"
        "    .then(data => {"
        "      console.log('Status data:', data);"
        "      document.getElementById('status').innerText = data;"
        "      document.getElementById('status').style.background = 'rgba(33, 150, 243, 0.3)';"
        "    })"
        "    .catch(error => {"
        "      console.error('Status error:', error);"
        "      document.getElementById('status').innerText = 'Error: ' + error;"
        "      document.getElementById('status').style.background = 'rgba(244, 67, 54, 0.3)';"
        "    });"
        "}"
        
        "speechInput.addEventListener('keypress', function(e) {"
        "  if (e.key === 'Enter') {"
        "    console.log('Enter key pressed');"
        "    makeSantaSpeak();"
        "  }"
        "});"
        
        "window.onload = function() {"
        "  console.log('Page fully loaded');"
        "  speechInput.focus();"
        "  getStatus();"
        "};"
        
        "setInterval(getStatus, 10000);"
        "</script>"
        "</body>"
        "</html>";
        
        httpd_resp_send(req, html, strlen(html));
        return ESP_OK;
    }

    static esp_err_t status_handler(httpd_req_t *req) {
        char response[256];
        snprintf(response, sizeof(response), 
                 "🎅 Santa is ready to speak! | 🎄 Christmas magic activated! | ✨ Web interface connected!");
        
        httpd_resp_send(req, response, strlen(response));
        return ESP_OK;
    }

    // Speech handler for web requests
    // Speech handler for web requests
    static esp_err_t speak_handler(httpd_req_t *req) {
        ESP_LOGI(TAG, "🎅 === SPEAK HANDLER CALLED ===");
        
        // Use smaller, stack-efficient buffers
        char* query = (char*)malloc(512);
        if (!query) {
            ESP_LOGE(TAG, "Failed to allocate query buffer");
            httpd_resp_send(req, "❌ Memory error", -1);
            return ESP_ERR_NO_MEM;
        }
        
        esp_err_t query_result = httpd_req_get_url_query_str(req, query, 512);
        ESP_LOGI(TAG, "🎅 Query string retrieval result: %s", esp_err_to_name(query_result));
        
        if (query_result == ESP_OK) {
            ESP_LOGI(TAG, "🎅 Query string: '%.100s%s'", query, strlen(query) > 100 ? "..." : "");
            
            char* text = (char*)malloc(256);
            if (!text) {
                free(query);
                ESP_LOGE(TAG, "Failed to allocate text buffer");
                httpd_resp_send(req, "❌ Memory error", -1);
                return ESP_ERR_NO_MEM;
            }
            
            esp_err_t param_result = httpd_query_key_value(query, "text", text, 256);
            ESP_LOGI(TAG, "🎅 Text parameter extraction result: %s", esp_err_to_name(param_result));
            
            if (param_result == ESP_OK) {
                ESP_LOGI(TAG, "🎅 Raw text: '%.50s%s' (len: %d)", 
                        text, strlen(text) > 50 ? "..." : "", (int)strlen(text));
                
                // Proper URL decode
                std::string decoded_text;
                for (int i = 0; text[i]; i++) {
                    if (text[i] == '+') {
                        decoded_text += ' ';
                    } else if (text[i] == '%' && text[i+1] && text[i+2]) {
                        // Convert hex to char
                        char hex[3] = {text[i+1], text[i+2], 0};
                        char decoded_char = (char)strtol(hex, NULL, 16);
                        decoded_text += decoded_char;
                        i += 2; // Skip the two hex digits
                    } else {
                        decoded_text += text[i];
                    }
                }
                
                ESP_LOGI(TAG, "🎅 Decoded text: '%s'", decoded_text.c_str());
                ESP_LOGI(TAG, "🎅 *** CALLING app.SpeakText() ***");
                
                // Get application instance and call SpeakText
                auto& app = Application::GetInstance();
                app.SpeakText(decoded_text);
                
                ESP_LOGI(TAG, "🎅 *** app.SpeakText() completed ***");
                
                // Send simple response
                const char* response = "🎅 Santa speech command sent successfully!";
                httpd_resp_send(req, response, strlen(response));
                
                free(text);
                free(query);
                ESP_LOGI(TAG, "🎅 === SPEAK HANDLER COMPLETED ===");
                return ESP_OK;
            }
            free(text);
        }
        
        free(query);
        ESP_LOGW(TAG, "🎅 === SPEAK HANDLER FAILED ===");
        httpd_resp_send(req, "❌ No text provided", -1);
        return ESP_OK;
    }
    // Add this new handler function in heysanta.cc
    // Replace the existing stop_handler with this version
    static esp_err_t stop_handler(httpd_req_t *req) {
        ESP_LOGI(TAG, "🛑 === STOP HANDLER CALLED ===");

        auto& app = Application::GetInstance();

        // 1) Mark the web control panel as inactive (same as MCP tool)
        app.SetWebControlPanelActive(false);

        // 2) Send immediate response to the browser
        const char* response = "🔴 Santa speech control panel closed. Web server will stop shortly.";
        httpd_resp_send(req, response, strlen(response));

        // 3) Defer the actual stop/reset work to the application task to avoid
        //    stopping the HTTP server from within its own handler thread.
        app.Schedule([&app]() {
            ESP_LOGI(TAG, "🛑 Stopping Santa and resetting to idle...");

            // Abort current speech and reset device state
            app.AbortSpeaking(kAbortReasonNone);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            app.SetDeviceState(kDeviceStateIdle);

            // Stop the speech web server safely from another task context
            if (speech_server != NULL) {
                ESP_LOGI(TAG, "🛑 Stopping Santa speech web server (async)...");
                httpd_stop(speech_server);
                speech_server = NULL;
                web_speech_active = false;
                ESP_LOGI(TAG, "🛑 Santa speech web server stopped");
            }

            ESP_LOGI(TAG, "🛑 Stop completed. Device reset to idle and control panel closed.");
        });

        ESP_LOGI(TAG, "🛑 === STOP HANDLER COMPLETED (response sent, shutdown scheduled) ===");
        return ESP_OK;
    }

    uint32_t unbiasedRandom3() {
        uint32_t r;
        const uint32_t upper_bound = 0xFFFFFFFF - (0xFFFFFFFF % 3);
        
        do {
            r = esp_random();
        } while (r >= upper_bound);
        
        return r % 3;
    }

    uint32_t unbiasedRandomRange(int min, int max) {
        // Calculate the range size
        uint32_t range = max - min + 1;
        
        // Determine the upper bound to reject values that cause bias
        uint32_t upper_bound = 0xFFFFFFFF - (0xFFFFFFFF % range);
        uint32_t r;
        
        do {
            r = esp_random(); // Get 32-bit random number from hardware
        } while (r >= upper_bound);
        
        return min + (r % range);
    }
    void dance()
    {
        for (int i = 1 ; i <= 3; i++) // dance for 3 times
        {
            uint32_t head_mode = unbiasedRandom3(); // Randomly choose head mode 
            uint32_t hip_mode = unbiasedRandom3(); // Randomly choose hip mode
            movement_type(1, head_mode, 1); // Head forward
            vTaskDelay(unbiasedRandomRange(1500, 5000) / portTICK_PERIOD_MS); // Wait for 5 second
            set_motor_A_speed(0); 
            vTaskDelay(unbiasedRandomRange(150, 1000) / portTICK_PERIOD_MS); // Wait for 1 second
            for (int j = 0; j < 3; j++) // Shake head 3 times
            {
                movement_type(2, hip_mode, 1); // Hip forward
                vTaskDelay(150 / portTICK_PERIOD_MS); // Wait for 0.5 second

                movement_type(2, hip_mode, -1); // Hip forward
                vTaskDelay(150 / portTICK_PERIOD_MS); // Wait for 0.5 second
            }
            set_motor_B_speed(0);
        }

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
        SetHeadSpeed(0);
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

    void start_speech_webserver(void) {
        if (speech_server != NULL) {
            ESP_LOGI(TAG, "Speech web server already running");
            return;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;
        config.lru_purge_enable = true;
        
        // INCREASE STACK SIZE TO PREVENT OVERFLOW
        config.stack_size = 8192;  // Increase from default 4096 to 8192
        config.task_priority = 5;  // Set appropriate priority
        config.max_uri_handlers = 10;  // Limit handlers
        config.max_resp_headers = 8;   // Limit response headers
        
        ESP_LOGI(TAG, "Starting Santa speech HTTP server on port: %d with stack size: %d", 
                config.server_port, config.stack_size);
                
        if (httpd_start(&speech_server, &config) == ESP_OK) {
            // Set URI handlers
            httpd_uri_t uri_get = {
                .uri       = "/",
                .method    = HTTP_GET,
                .handler   = speech_control_page_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(speech_server, &uri_get);

            httpd_uri_t uri_status = {
                .uri       = "/status",
                .method    = HTTP_GET,
                .handler   = status_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(speech_server, &uri_status);

            // Add speech handler
            httpd_uri_t uri_speak = {
                .uri       = "/speak",
                .method    = HTTP_GET,
                .handler   = speak_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(speech_server, &uri_speak);
             httpd_uri_t uri_stop = {
                .uri       = "/stop",
                .method    = HTTP_GET,
                .handler   = stop_handler,
                .user_ctx  = NULL
            };
            httpd_register_uri_handler(speech_server, &uri_stop);
            ESP_LOGI(TAG, "🎅 Santa speech web server started successfully!");
            web_speech_active = true;
            
        } else {
            ESP_LOGE(TAG, "Failed to start Santa speech web server!");
        }
    }

    void stop_speech_webserver(void) {
        if (speech_server != NULL) {
            httpd_stop(speech_server);
            speech_server = NULL;
            web_speech_active = false;
            ESP_LOGI(TAG, "Santa speech web server stopped");
        }
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        mcp_server.AddTool("self.chassis.dance", "跳舞", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Dance command received");
            dance();
            return true;
        });
        
        mcp_server.AddTool("self_chassis_shake_body", "摇头", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Head shake command received");
            HeadShakeOnly();
            return true;
        });
        
        // NEW MCP TOOL: Open Santa speech control panel
        mcp_server.AddTool("open_santa_speech_panel", "开启圣诞老人语音控制面板", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "🎅 Opening Santa speech control panel...");
            
            if (!WifiStation::GetInstance().IsConnected()) {
                ESP_LOGW(TAG, "WiFi not connected, cannot start web server");
                return "❌ WiFi not connected. Please connect to WiFi first.";
            }
            
            // Set the web control panel flag in Application
            auto& app = Application::GetInstance();
            app.SetWebControlPanelActive(true);
            
            // Start the web server
            start_speech_webserver();
            
            // Get IP address
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(netif, &ip_info);
            
            char response[512];
            snprintf(response, sizeof(response), 
                    "🎅 Santa Speech Control Panel Started!\n"
                    "📱 Open your browser and go to:\n"
                    "🌐 http://" IPSTR ":8080\n"
                    "🗣️ Type messages for Santa to speak\n"
                    "🎄 Includes preset Christmas messages\n"
                    "✨ Spread Christmas joy with Santa's voice!",
                    IP2STR(&ip_info.ip));
            
            ESP_LOGI(TAG, "%s", response);
            return response;
        });
        
        mcp_server.AddTool("self_chassis_shake_hip", "摇屁股", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Hip shake command received");
            HipShakeOnly();
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
        mcp_server.AddTool("close_santa_speech_panel", "关闭圣诞老人语音控制面板", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "🎅 Closing Santa speech control panel...");
            
            // Clear the web control panel flag in Application
            auto& app = Application::GetInstance();
            app.SetWebControlPanelActive(false);
            
            // Force reset the application to idle state
            app.Schedule([&app]() {
                app.SetDeviceState(kDeviceStateIdle);
                
                // Clear any audio queues
                // Note: You might need to make these methods public or add a public method to do this
                ESP_LOGI(TAG, "🎅 Resetting device state after closing control panel");
            });
            
            stop_speech_webserver();
            return "🔴 Santa speech control panel closed. Web server stopped.";
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
        config.ledc_channel = LEDC_CHANNEL_5;
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