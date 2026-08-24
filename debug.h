#pragma once

#include <stdbool.h>
#include <stdio.h>

extern bool debug_enabled;

// Checks DEBUG_TX_PIN for presence of a serial adapter, sets the global debug_enabled var accordingly.
// If debug is enabled, initializes the serial UART for printing via LOG().

// Sets debug_enabled to true if any key is pressed (sent down the DEBUG_TX_PIN to the ATtiny) within
// the given delay. Debug output is only formatted and sent if such a keypress happens while this
// function runs during startup.
void EnableDebugOnSerialInput(uint16_t max_delay_ms);

// If debug is off, this evaluates instantly to false and skips the printf entirely.
#define LOG(format, ...) do { \
    if (debug_enabled) { \
      printf(format, ##__VA_ARGS__); \
    } \
  } while(0)
