#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvFT6X36.hpp>
#include <TCA9554.h>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ============================================================================
// Waveshare ESP32-S3-Touch-LCD-3.5  (the NON-"B" variant)
//   Display : ST7796 320x480 IPS LCD via standard 4-wire SPI (has a DC pin).
//             Run 480x320 LANDSCAPE via the ST7796 driver's HW rotation
//             (no Canvas needed — ST7796 MADCTL supports axis exchange).
//   Touch   : FocalTech FT6336 capacitive, I2C addr 0x38 (SensorLib
//             TouchDrvFT6X36). NOT the AXS15231B 0x3B used by the "3.5B".
//   IO exp  : TCA9554 @ 0x20 — channel 1 drives the LCD reset pulse.
//   PMU     : AXP2101 @ 0x34 — LCD power rails + battery + PWR key.
//   IMU     : QMI8658 (present; not used for rotation, fixed landscape).
//   Shared I2C bus: SDA=8, SCL=7 (touch + TCA9554 + AXP2101 + QMI8658).
// ============================================================================

// ---- Logical (LVGL/UI) resolution: landscape ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  320

// ---- Native panel resolution (portrait, before rotation) ----
#define PANEL_W     320
#define PANEL_H     480

// ST7796 hardware rotation that yields 480x320 landscape.
// 1 = 90° CW, 3 = 90° CCW. Flip 1<->3 if the image is upside-down.
#define LCD_ROTATION  1

// ---- ST7796 standard SPI pins ----
#define LCD_SPI_DC    3
#define LCD_SPI_CS    -1   // tied / not on a GPIO
#define LCD_SPI_SCK   5
#define LCD_SPI_MOSI  1
#define LCD_SPI_MISO  2
#define LCD_RST       -1   // reset is via TCA9554 channel 1
#define LCD_BL        6    // backlight enable (active high)

// ---- Shared I2C bus ----
#define IIC_SDA       8
#define IIC_SCL       7

// ---- TCA9554 I/O expander ----
#define TCA9554_ADDR   0x20
#define TCA_LCD_RST_CH 1    // expander channel toggled for the LCD reset pulse

// ---- Touch (FocalTech FT6336 @ 0x38) ----
// Address + protocol handled by SensorLib's TouchDrvFT6X36.

// ---- PMU (AXP2101 via same I2C) ----
#define AXP2101_ADDR   0x34

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus  *bus;
extern Arduino_GFX      *gfx;     // Arduino_ST7796 (HW-rotated to landscape)
extern TouchDrvFT6X36    touch;
extern TCA9554           tca;
extern XPowersPMU        pmu;
extern SensorQMI8658     imu;
