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
#define RADIO_FREQUENCY_HZ   868350000UL
//                           MMMkkk000UL

#define DEBOUNCE_MS 10   // tune to your reed switch's real bounce time

// Power transmission output from minimum 0 (+2 dBm) to maximum 15 (+17 dBm).
// Lower values save power (maybe try a simple dipole antenna instead of the coiled one).
// Too high power close to the receiver may also overwhelm it.
// (Not configurable: The SX1276 +20 dBm mode needs 15 here, plus RegPaDac (0x4D) set to 0x87, plus changes to RegOcp (0x0B) to raise the over-current-protection.)
#define RADIO_POWER 15

// When using OpenMQTTGateway versions that suffer from bug https://github.com/1technophile/OpenMQTTGateway/issues/2356,
// ensure that sensor updates are at least 3 sec apart from one another, else they can get ignored.
#define MIN_UPDATE_DELAY_MS 3100
