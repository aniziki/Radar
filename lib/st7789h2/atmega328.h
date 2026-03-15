// ESP32 pin shim — replaces the AVR port-register macros from the
// Crystalfontz ATmega328 demo. Pin numbers come from platformio.ini.

#pragma once
#include <Arduino.h>

#define CLR_CS    digitalWrite(PIN_DISP_CS,  LOW)
#define SET_CS    digitalWrite(PIN_DISP_CS,  HIGH)
#define CLR_DC    digitalWrite(PIN_DISP_DC,  LOW)
#define SET_DC    digitalWrite(PIN_DISP_DC,  HIGH)
#define CLR_RESET digitalWrite(PIN_DISP_RST, LOW)
#define SET_RESET digitalWrite(PIN_DISP_RST, HIGH)

// writeCommand/writeData are used interchangeably with SPI_sendCommand/SPI_sendData
// in st7789h2.cpp — forward-declared here, defined there.
void SPI_sendCommand(uint8_t command);
void SPI_sendData(uint8_t data);
#define writeCommand SPI_sendCommand
#define writeData    SPI_sendData