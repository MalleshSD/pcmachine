# STM32 Billing Machine

## 📌 Project Overview

A real-time embedded billing machine developed using the STM32F103C8T6 microcontroller.

The system allows users to select products, enter quantities, calculate the total amount and display billing information through a TFT display.

## 🔧 Hardware Used

- STM32F103C8T6
- ILI9488 TFT Display
- 4x4 Matrix Keypad
- AT24C512 EEPROM
- 16x2 I2C LCD
- TM1637 7-Segment Display
- Buzzer

## 💻 Software & Tools

- Embedded C
- STM32CubeIDE
- STM32 HAL
- ST-Link
- Proteus

## 🔌 Communication Protocols

- SPI – TFT Display
- I2C – EEPROM and LCD
- GPIO – Keypad and control signals

## ⚙️ Main Features

- Product selection
- Quantity entry
- Automatic total calculation
- Bill generation
- EEPROM data storage
- TFT graphical interface
- LCD information display
- Buzzer indication

## 📂 Project Structure

```text
pcmachine/
├── Core/
├── Drivers/
├── Debug/
├── .settings/
├── pcmachine.ioc
├── STM32F103C8TX_FLASH.ld
└── README.md
