// https://github.com/atricat/wh51-emulation-sx1276
//
// Emulate a WH51 moisture sensor using an ATtiny1614/ATtiny1617 and a SX1276 radio module.
// This code transmits the state of two switches/reed contacts as a fake moisture value, but of course you could add other sensors, e.g. an actual moisture sensor.
// In the Arduino UI, select an appropriate clock speed. 20 MHz is fine, but less to save power for a battery-powered node.

#include <Arduino.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <stdint.h>

#include "config.h"
#include "debug.h"
#include "sx1276.h"

#define PINCTRL_OF(p) getPINnCTRLregister(digitalPinToPortStruct(p), digitalPinToBitPosition(p))

uint16_t BatteryMilliVolt(void) {
  // Measure 1.1V internal ref against VDD
  VREF.CTRLA = VREF_ADC0REFSEL_1V1_gc;
  ADC0.MUXPOS = ADC_MUXPOS_INTREF_gc;
  ADC0.CTRLC = ADC_PRESC_DIV16_gc | ADC_REFSEL_VDDREF_gc;
  ADC0.CTRLA = ADC_ENABLE_bm;

  ADC0.COMMAND = ADC_STCONV_bm;
  while (!(ADC0.INTFLAGS & ADC_RESRDY_bm));

  uint16_t adc_raw = ADC0.RES;
  ADC0.CTRLA = 0; // Disable ADC

  return (uint16_t)((1126400UL) / adc_raw);
}

// Low-power sleep delay helper using IDLE mode
void SleepMsec(uint8_t ms) {
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  uint32_t start_time = millis();
  while (millis() - start_time < ms) {
    sleep_cpu(); // Halts CPU clock; background millis timer ISR wakes it every 1ms
  }
  sleep_disable();
}

uint8_t ChecksumAdd(const uint8_t *data, uint8_t len) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < len; i++) sum += data[i];
  return (uint8_t)(sum & 0xFF);
}

// Write the last 3 bytes of the ATtiny1614 factory serial number.
// Result is SERNUM7:SERNUM8:SERNUM9.
static void GetDeviceId(uint8_t* out) {
  #if defined(DEVICE_ID_0) && defined(DEVICE_ID_1) && defined(DEVICE_ID_2)
  *out++ = DEVICE_ID_0;
  *out++ = DEVICE_ID_1;
  *out++ = DEVICE_ID_2;
  #elif defined(SIGROW)
  // ATtiny1614: SERNUM7..9 are signature-row offsets 0x0A..0x0C.
  *out++ = SIGROW.SERNUM7;
  *out++ = SIGROW.SERNUM8;
  *out++ = SIGROW.SERNUM9;
  #elif defined(__AVR_ATtiny1614__)
  // ATtiny1614 SIGROW is mapped at 0x1100.
  // SERNUM7/8/9 are offsets 0x0A/0x0B/0x0C.
  volatile const uint8_t *sigrow = (volatile const uint8_t *)0x1100;
  *out++  = sigrow[0x0A];
  *out++  = sigrow[0x0B];
  *out++  = sigrow[0x0C];
  #else
#error "Unsupported ATtiny1614 toolchain: no SIGROW access method"
  #endif
}

// CRC8 calculation (FineOffset Poly 0x31)
uint8_t Crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80) crc = (crc << 1) ^ 0x31;
      else crc <<= 1;
    }
  }
  return crc;
}

// Transmit FSK packet.
void SendPacket(uint8_t current_reed_state) {
  uint8_t payload[14];
  uint8_t* out = payload;

  *out++ = 0x51; // WH51 family code
  GetDeviceId(out);
  out += 3;

  uint8_t boost = 0;                      // 0-7, real sensors set 7 on a moisture/state change
  // Fake WH51 scales the ATTiny 3.3V supply voltage to 1.5V.
  uint16_t wh51_battery_mV = BatteryMilliVolt() * 15 / 33;
  uint8_t battery_code = (uint8_t)((wh51_battery_mV + 50) / 100);
  if (battery_code > 31)
    battery_code = 31;
  *out++ = (boost << 5) | battery_code;

  *out++ = 0x7F; // fixed

  // Repurpose "moisture" (0-100) to carry the reed state.
  // 0 = both open, 1 = one closed, 2 = other closed, 3 = both closed.
  *out++ = current_reed_state;

  uint16_t ad_raw = 56 + 10 * current_reed_state; // no real AD value - just make something up.
  *out++ = 0xF8 | ((ad_raw >> 8) & 0x01);
  *out++ = ad_raw & 0xFF;

  *out++ = 0xFF;
  *out++ = 0xFF;
  *out++ = 0xFF;

  *out++ = Crc8(payload, 12); // CRC over bytes 0-11
  *out++ = ChecksumAdd(payload, 13); // sum of bytes 0-12

  if (debug_enabled) {
    uint8_t* payload_dbg = payload;
    LOG("[TX] Payload:");
    while (payload_dbg < out) LOG(" %02x", *payload_dbg++);
    LOG("\n");
  }

  // Transmit the data.
  SX1276_Standby();
  SX1276_SendPacket(payload, out);
  SX1276_WaitForTxDone(50);

  // Re-transmit in case the receiver didn't catch the first transmission.
  SleepMsec(36);
  SX1276_SendPacket(payload, out);
  SX1276_WaitForTxDone(50);
  SX1276_Sleep();
}

static inline uint8_t ReadReedState(void) {
  uint8_t state = 0;
  if (digitalRead(REED1_PIN) == LOW) state |= 0x01;
  if (digitalRead(REED2_PIN) == LOW) state |= 0x02;
  return state;
}

#define DEBOUNCE_MS 10   // tune to your reed switch's real bounce time

// Blocks (in low-power IDLE sleep) until the reed state has been
// unchanged for DEBOUNCE_MS straight, then returns that settled state.
uint8_t DebounceReedState() {
  uint8_t last_sample = ReadReedState();
  uint32_t stable_since = millis();

  while ((millis() - stable_since) < DEBOUNCE_MS) {
    SleepMsec(1); // low-power 1ms poll tick
    uint8_t sample = ReadReedState();
    if (sample != last_sample) {
      last_sample = sample;
      stable_since = millis();
    }
  }
  return last_sample;
}

// Interrupt service routine.
ISR(PORTA_PORT_vect) {
  // Just wake the CPU - all state logic lives in the main loop.
  PORTA.INTFLAGS = digitalPinToBitMask(REED1_PIN) | digitalPinToBitMask(REED2_PIN);
}

void DeepSleepAllowUdpi() {
  if (!debug_enabled) {
    digitalWrite(LED_PIN, LOW);
    SLPCTRL.CTRLA = SLPCTRL_SMODE_PDOWN_gc | SLPCTRL_SEN_bm;
    sleep_cpu();
    SLPCTRL.CTRLA &= ~SLPCTRL_SEN_bm;
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  uint16_t timeout = 1000;
  while (!(USART0.STATUS & USART_TXCIF_bm) && --timeout) {
    _delay_us(10);
  }
  USART0.STATUS = USART_TXCIF_bm;

  // Prepare for sleep. Do NOT touch TXEN, leave it enabled to avoid wakeup UART problem.
  PORTB.DIRCLR = DEBUG_TX_PIN;
  PORTB.PIN2CTRL = 0;

  digitalWrite(LED_PIN, HIGH);
  SLPCTRL.CTRLA = SLPCTRL_SMODE_PDOWN_gc | SLPCTRL_SEN_bm;
  sleep_cpu();
  SLPCTRL.CTRLA &= ~SLPCTRL_SEN_bm;
  digitalWrite(LED_PIN, LOW);

  // Post-wakeup init and UPDI halt window.
  delay(10);
  PORTB.DIRSET = DEBUG_TX_PIN; // TXEN was never disabled - nothing else to restore
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  delay(3000); //
  digitalWrite(LED_PIN, LOW);
  delay(100);
  pinMode(REED1_PIN, INPUT_PULLUP);
  pinMode(REED2_PIN, INPUT_PULLUP);

  Check_Debug_AutoDetect();

  // Configure Reed Switches (PA1, PA2) - Active LOW, Both Edges
  *PINCTRL_OF(REED1_PIN) |= PORT_PULLUPEN_bm | PORT_ISC_BOTHEDGES_gc;
  *PINCTRL_OF(REED2_PIN) |= PORT_PULLUPEN_bm | PORT_ISC_BOTHEDGES_gc;

  // For a battery-powered node, avoid that floating/unused digital inputs leak a little current.
  PORTA.PIN7CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTB.PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTB.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;

  uint8_t device_id[3];
  GetDeviceId(device_id);
  LOG("Using Fine Offset device ID %02x%02x%02x\n", device_id[0], device_id[1], device_id[2]);

  SPI_Init();
  SX1276_CheckPresence();
  SX1276_Init_FineOffset();
  SX1276_Sleep();
}

void loop(void) {
  // LoopRx(); // Uncomment to listen to genuine WH51 sensors.
  sei();
  uint8_t last_reported_state = 0xff; // Always send state on startup.

  while (true) {
    uint8_t settled_state = DebounceReedState();
    if (settled_state == last_reported_state) {
      if (debug_enabled) _delay_ms(10); // Let UART finish printing
      DeepSleepAllowUdpi(); // Sleeps until any reed edge wakes it
    } else {
      LOG("[WAKE] State: %02x\n", settled_state);
      SendPacket(settled_state);
      last_reported_state = settled_state;
    }
  }
}
