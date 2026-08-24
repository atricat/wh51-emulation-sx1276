#include <Arduino.h>
#include <stdio.h>
#include <util/delay.h>

#include "debug.h"

#include "config.h"

// Global debug flag.
bool debug_enabled = false;

// USART0 on PB2
#define USART0_BAUD_RATE(BAUD_RATE) ((float)(F_CPU * 64 / (16 * (float)BAUD_RATE)) + 0.5)

// Avoids blocking on logs if nobody is listening on the UART.
int UART_PrintChar(char c, FILE *stream) {
  // Exit immediately if debugging is disabled
  if (!debug_enabled) return 0;

  // Format newlines into CRLF for standard serial monitors
  if (c == '\n') {
    UART_PrintChar('\r', stream);
  }

  // 3. Wait up to 20ms for hardware buffer slot to become available
  uint32_t start_time = millis();
  while (!(USART0.STATUS & USART_DREIF_bm)) {
    if (millis() - start_time >= 20) {
      // TIMEOUT: Buffer is stuck/unresponsive.
      // Clear transmission flags to reset hardware buffer state...
      USART0.STATUS = USART_TXCIF_bm;
    }
  }

  // Send character
  USART0.TXDATAL = c;
  return 0;
}

// Set up a standard I/O stream structure assigned to our custom print function
static FILE uart_stdout = FDEV_SETUP_STREAM(UART_PrintChar, NULL, _FDEV_SETUP_WRITE);

void UART_Init(uint32_t baud) {
  // Route USART0 TX internally to PA0 (Physical Pin 7)
  //PORTMUX.CTRLB |= PORTMUX_USART0_ALTERNATE_gc;

  PORTB.DIRSET = DEBUG_TX_PIN; // Set PB2 as output

  // Calculate BAUD depending on CPU speed.
  USART0.BAUD = (uint16_t)(((float)F_CPU * 64.0) / (16.0 * (float)baud) + 0.5);

  // Enable TX
  USART0.CTRLB = USART_TXEN_bm;

  stdout = &uart_stdout;
}

// Single-Pin USB Auto-Detect
void EnableDebugOnSerialInput(uint16_t max_delay_ms) {
  PORTB.DIRCLR = DEBUG_TX_PIN;
  PORTB.PIN2CTRL = PORT_PULLUPEN_bm; // deterministic idle-HIGH baseline
  uint32_t start_time = millis();
  while (millis() - start_time < max_delay_ms) {
    if (!(PORTB.IN & DEBUG_TX_PIN)) {
      // delay(200);
      debug_enabled = true;
      // Wait for the pin to be idle for 100 ms, i.e. until any incoming keypress has been
      // transmitted and will not interfere with us outputting the first log message.
      uint32_t last_change = millis();
      bool last_state = PORTB.IN & DEBUG_TX_PIN;
      while (millis() - last_change < 100) {
        bool current_state = PORTB.IN & DEBUG_TX_PIN;
        if (current_state != last_state) {
          last_change = millis();
          last_state = current_state;
        }
      }
      UART_Init(9600);
      LOG("GPIO pin pulled low - serial logging enabled!\n");
      return;
    }
    _delay_us(10);
  }
  debug_enabled = false;
  // Disable GPIO since we are not using it for debug output.
  #if DEBUG_TX_PIN != PIN2_bm
  #error "Adapt the line below to the changed DEBUG_TX_PIN"
  PORTB.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  #endif
}
