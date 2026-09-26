# E-SentriCity

### Smart Urban Surveillance and Emergency Detection System

E-SentriCity is an IoT-based smart surveillance and emergency detection system designed to improve situational awareness in urban environments.

The system combines distributed ESP32 sensor nodes, wireless nRF24L01 communication, an ESP32-CAM for visual monitoring, and a Raspberry Pi for centralized data reception and processing.

---

## Overview

Traditional surveillance systems mainly depend on continuous video monitoring. E-SentriCity adds an additional layer of intelligent event detection by combining sensor-based threat detection with on-demand visual verification.

The system continuously monitors the environment using sensors such as PIR and sound detection modules. When a potential threat is detected, the sensor information is transmitted wirelessly to a Raspberry Pi. The Raspberry Pi can then access the ESP32-CAM over Wi-Fi for visual monitoring.

This approach helps reduce unnecessary continuous camera streaming while providing visual information when an event requires verification.

---

## Key Features

- Real-time motion detection using PIR sensors
- Sound/activity detection
- Wireless communication using nRF24L01
- Raspberry Pi based central monitoring
- ESP32-CAM based visual monitoring
- Wi-Fi camera streaming
- Event-based camera activation
- Modular and scalable sensor-node architecture
- Low-cost hardware implementation

---

## System Architecture

```text
                 ┌──────────────────────┐
                 │     ESP32 SENSOR     │
                 │        NODE(S)        │
                 │                      │
                 │  PIR Sensor          │
                 │  Sound Sensor        │
                 └──────────┬───────────┘
                            │
                            │ nRF24L01
                            │
                            ▼
                 ┌──────────────────────┐
                 │     RASPBERRY PI     │
                 │                      │
                 │  nRF24 Receiver      │
                 │  Event Processing    │
                 │  Central Monitoring  │
                 └──────────┬───────────┘
                            │
                            │ Wi-Fi
                            │
                            ▼
                 ┌──────────────────────┐
                 │      ESP32-CAM       │
                 │                      │
                 │   Camera Module      │
                 │   HTTP Server        │
                 │   Live MJPEG Stream  │
                 └──────────────────────┘
