#include <Arduino.h>
#include <stdio.h>

#include "debug.h"

#include "config.h"

// Global debug flag.
bool debug_enabled = false;

// --- UART DEBUG DRIVER (USART0 on PB2) ---
#define USART0_BAUD_RATE(BAUD_RATE) ((float)(F_CPU * 64 / (16 * (float)BAUD_RATE)) + 0.5)

// Avoids blocking on logs if nobody is listening on the UART.
int UART_PrintChar(char c, FILE *stream) {
  // 1. Exit immediately if debugging is disabled
  if (!debug_enabled) return 0;

  // 2. Format newlines into CRLF for standard serial monitors
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
  // 2. Route USART0 TX internally to PA0 (Physical Pin 7)
  //PORTMUX.CTRLB |= PORTMUX_USART0_ALTERNATE_gc;

  // 3. Set PA0 as output
  //PORTA.DIRSET = PIN0_bm;
  PORTB.DIRSET = DEBUG_TX_PIN; // Set PB2 as output

  // 4. Calculate BAUD for 20MHz clock
  USART0.BAUD = (uint16_t)(((float)F_CPU * 64.0) / (16.0 * (float)baud) + 0.5);

  // 5. Enable TX
  USART0.CTRLB = USART_TXEN_bm;

  stdout = &uart_stdout;
}

// Single-Pin USB Auto-Detect
void Check_Debug_AutoDetect() {
  // 1. Leave PB2 as high-impedance input with internal pull-up disabled
  PORTB.DIRCLR = DEBUG_TX_PIN;
  PORTB.PIN2CTRL = 0x00;

  // 2. Allow extra settling time for external USB Serial adapters to power up
  delay(10);

  // 3. Sample the line state: Serial RX lines sit HIGH when idle
  if (PORTB.IN & DEBUG_TX_PIN) {
    debug_enabled = true;
    UART_Init(9600);
    LOG("USB-serial adapter auto-detected!\n");
  } else {
    debug_enabled = false;
    PORTB.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  }
}
