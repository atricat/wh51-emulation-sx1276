#pragma once

#include <stdbool.h>
#include <stdio.h>

void UART_Init(uint32_t baud);
void Check_Debug_AutoDetect();

// If debug is off, this evaluates instantly to false and skips the printf entirely.
extern bool debug_enabled;
#define LOG(format, ...) do { \
    if (debug_enabled) { \
      printf(format, ##__VA_ARGS__); \
    } \
  } while(0)
