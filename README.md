# ESP-IDF Window Roller

An ESP-IDF based driver for a smart window roller. This project is a significantly upgraded and more feature-rich successor to my previous [Arduino Core version](https://github.com/Knuti-exe/window_roller).

## Key Features

* **Stepper Motor Control:** Precise management of the window roller mechanism.
* **BLE to MQTTS Gateway:** Actively listens for BLE advertising packets (e.g., from environmental sensors) and securely forwards the data using the MQTT over TLS protocol.
* **Secure OTA Updates:** Supports Over-the-Air firmware updates fetched directly from a local HTTPS server.
* **Remote Logging:** Streams system logs and function states to dedicated MQTT topics for easy remote debugging and monitoring.

## Usage & Integration

The device can be easily integrated into existing smart home setups or controlled via mobile apps like *MQTT Dashboard*. It is designed to work entirely within a local network, securely connecting to your local MQTT broker and HTTPS update server.
