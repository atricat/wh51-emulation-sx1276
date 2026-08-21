#pragma once

// By default, auto-generates an ID from the ATtiny's serial - override here with a fixed ID.
// #define DEVICE_ID_0  0x0F   // 3-byte ID, first byte is always 0xf for genuine Fine Offset / Ecowitt
// #define DEVICE_ID_1  0xDE
// #define DEVICE_ID_2  0xAD
#define REED1_PIN    PIN_PA5   // physical pin 3
#define REED2_PIN    PIN_PA6   // physical pin 4
#define NSS_PIN      PIN4_bm   // PA4 (physical pin 2) - default SPI0 SS pin, used as manual/software CS
#define LED_PIN      PIN_PB1   // physical pin 8
#define DEBUG_TX_PIN PIN2_bm   // PB2 (Hardware USART0 TX default pin) for serial debug output. We auto-detect whether a serial chip is attached.

// Change just this one line to retarget the frequency.
#define RADIO_FREQUENCY_HZ   868350000UL   // measured via AFC, not assumed
//                           MMMkkk000UL
