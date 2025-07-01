#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Use Simplex I2S mode like the working ESP32 breadboard
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

// Microphone I2S pins (INMP441) - match your hardware connections
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_11   // Word Select
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_10   // Serial Clock
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_4    // Serial Data In

// Speaker I2S pins (MAX98357) - match your hardware connections  
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_17   // Serial Data Out
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_16   // Bit Clock
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_15   // Left/Right Clock

#else

// Duplex mode (if needed)
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_15
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_4
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_17

#endif

// Button definitions
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_NC   // Not connected on TurtleBot
#define ASR_BUTTON_GPIO         GPIO_NUM_NC   // Not connected on TurtleBot
#define BUILTIN_LED_GPIO        GPIO_NUM_2

// Display configuration (SSD1306 OLED)
#define DISPLAY_SDA_PIN GPIO_NUM_7
#define DISPLAY_SCL_PIN GPIO_NUM_8
#define DISPLAY_WIDTH   128
#define DISPLAY_I2C_ADDR 0x3C  // ADDED THIS LINE

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#else
#define DISPLAY_HEIGHT  64  // Default to 64
#endif

#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

// Servo motor pins
#define SERVO_LEFT_FRONT_PIN     GPIO_NUM_38
#define SERVO_LEFT_BACK_PIN      GPIO_NUM_39
#define SERVO_RIGHT_FRONT_PIN    GPIO_NUM_40
#define SERVO_RIGHT_BACK_PIN     GPIO_NUM_41
#define SERVO_HEAD_PIN           GPIO_NUM_42

// Servo offsets
#define SERVO_OFFSET_LEFT_FRONT  0
#define SERVO_OFFSET_LEFT_BACK   -3
#define SERVO_OFFSET_RIGHT_FRONT -3
#define SERVO_OFFSET_RIGHT_BACK  2
#define SERVO_OFFSET_HEAD        0

// Unused definitions for compatibility
#define LAMP_GPIO GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_