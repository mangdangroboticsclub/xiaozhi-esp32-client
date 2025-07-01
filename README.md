# Mangdang Turtlebot with Xiaozhi Client Setup Guide

<div style="text-align: center; margin: 20px 0;">
    <img src="https://drive.google.com/uc?export=view&id=16KVJ1fb496YkroQcYdX7NpUUEJHuMKzh" alt="Mangdang Turtlebot Setup" style="width: 450px; height: auto; border-radius: 8px;">
</div>

This guide will walk you through setting up the Xiaozhi assistant client on your Mangdang Turtlebot using ESP32-S3.

## Prerequisites

- Mangdang Turtlebot with ESP32-S3
- Computer with internet connection
- USB cable for flashing

## Installation Steps

### 1. Install ESP-IDF

Download and install ESP-IDF from  (For windows):
```
https://dl.espressif.com/dl/esp-idf/?idf=4.4
```

### 2. Open ESP-IDF PowerShell

After installation, launch the ESP-IDF PowerShell from your Start menu.

### 3. Clone and Setup the Project

Run the following commands in the ESP-IDF PowerShell:

```bash
git clone https://github.com/Bariest/xiaozhi-esp32-client.git
cd xiaozhi-esp32-client
git checkout Turtle
```

### 4. Configure the Target

Set the target device to ESP32-S3:
```bash
idf.py set-target esp32s3
```

### 5. Configure Project Settings

Open the configuration menu:
```bash
idf.py menuconfig
```

Navigate to **Xiaozhi Assistant** settings and configure:
- **Default Language**: Choose Chinese or English
- **Board Type**: **IMPORTANT** - Change to "Mangdang Turtlebot"
- **Audio Settings**: Enable both "AFE" and "Audio Noise Reduction"

### 6. Build the Project

Compile the project (this takes 1-2 minutes as it generates ~2000 files):
```bash
idf.py build
```

### 7. Find Your Device Port

Before flashing, identify your device's COM port:
1. Open **Device Manager**
2. Expand **Ports (COM & LPT)**
3. Look for "USB Serial..." entry
4. Note the COM port number (e.g., COM3, COM5, etc.)

### 8. Flash the Device

Flash the compiled code to your device:
```bash
idf.py -p COM[X] flash
```
Replace `[X]` with your actual COM port number.

### 9. Monitor the Device

Start monitoring the device output:
```bash
idf.py monitor
```

## WiFi Configuration

### 10. Initial Setup

The robot will announce: "Please enter your wifi configuration mode"

### 11. Connect to Robot's WiFi

1. On your device, search for WiFi networks
2. Look for a network starting with "xiaozhi..."
3. Connect to this network

### 12. Configure Internet WiFi

1. Open a web browser and go to: `https://192.168.4.1`
2. Select your home WiFi network
3. Enter your WiFi password
4. Submit the configuration

## Device Registration

### 13. Register with Xiaozhi Service

1. Go to [xiaozhi.me](https://xiaozhi.me)
2. Navigate to **Console** > **Add Device**
3. Unplug and reconnect your device
4. The device will display a unique code
5. Enter this code to register your device
6. Customize your AI agent settings as needed

### 14. Monitor Operation

You can view real-time input and output logs in the ESP-IDF monitor window.

## Troubleshooting

- If the device doesn't appear in Device Manager, try a different USB cable
- Ensure you're using the correct COM port number
- If WiFi configuration fails, restart from step 10
- Check the monitor output for any error messages

Your Mangdang Turtlebot is now ready to use with the Xiaozhi AI assistant!