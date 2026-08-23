#pragma once

#include <stdbool.h>
#include <stdio.h>

extern bool debug_enabled;

// Checks DEBUG_TX_PIN for presence of a serial adapter, sets the global debug_enabled var accordingly.
// If debug is enabled, initializes the serial UART for printing via LOG().
void Check_Debug_AutoDetect();

// If debug is off, this evaluates instantly to false and skips the printf entirely.
#define LOG(format, ...) do { \
    if (debug_enabled) { \
      printf(format, ##__VA_ARGS__); \
    } \
  } while(0)
