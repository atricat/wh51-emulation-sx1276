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
  // 0 = both open, 10 = one closed, 20 = other closed, 30 = both closed.
  *out++ = current_reed_state * 10;

  uint16_t ad_raw = 56 + 10 * current_reed_state; // no real AD value - just make something up.
  *out++ = 0xF8 | ((ad_raw >> 8) & 0x01);
  *out++ = ad_raw & 0xFF;

  *out++  = 0xFF;
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
  SX1276_SendPacket(payload, payload+14);
  SX1276_WaitForTxDone(50);
  SX1276_Sleep();
 

/*
  SX1276_WriteReg(REG_PAYLOAD_LENGTH, 14);

  // Dynamic timing tracker: Initial wakeup needs 1ms, second burst loop needs 30ms
  uint16_t standby_duration_ms = 1;

  // --- BURST LOOP: Send multiple times ---
  for (uint8_t burst = 0; burst < 2; burst++) {
    // for (uint8_t burst = 0; burst < 10*4; burst++) {

    // 1. Force the radio into Standby mode to clear FIFO frame pointers
    SX1276_WriteReg(REG_OP_MODE, 0x01); // Standby

    // 2. Power-Optimized Gap/Settle Delay (1ms first loop, 30ms second loop)
    SleepMsec(standby_duration_ms);

    // 3. Refill the transmission FIFO pipeline
    PORTA.OUTCLR = NSS_PIN;
    SPI_Transfer(REG_FIFO | 0x80);
    for (uint8_t i = 0; i < 14; i++) {
      SPI_Transfer(payload[i]); // Pushes indexed bytes to SPI
    }
    PORTA.OUTSET = NSS_PIN;

    // 4. Trigger FSK Transmit (Switch to TX mode)
    // Gemini argues for 0x0b; "Gaussian Modulation Shaping: Fine Offset transmitters utilize Gaussian filtering"
    SX1276_WriteReg(REG_OP_MODE, 0x03); // TX

    // 5. Sleep the CPU for 15ms while the radio pushes the packet over the air
    // SleepMsec(15);
    SX1276_WaitForTxDone(50);

    // 6. Update configuration values for the next burst pass
    // 36 ms delay according to https://github.com/merbanan/rtl_433/issues/2955
    standby_duration_ms = 36;
  }

  // Final Sequence: Lock down radio back to deep sleep mode
  SX1276_Sleep();*/
}

#if 0
#define REG_RXCONFIG   0x0D
#define REG_AFCBW      0x13
#define REG_AFCMSB     0x1B
#define REG_AFCLSB     0x1C

void SX1276_EnableAfc(void) {
  // AfcAutoOn: automatically run AFC on every new packet reception.
  // I'm fairly confident this is bit 4 of RegRxConfig based on the
  // standard SX1276 driver layout - worth a quick cross-check against
  // your toolchain's header if you want to be extra sure before relying on it.
  SX1276_WriteReg(REG_RXCONFIG, 0x1E); // AfcAutoOn=1, AgcAutoOn=1, RxTrigger=preamble+RSSI

  // AFC capture bandwidth should be WIDER than your RxBw, to give AFC
  // enough headroom to find an offset before the narrower channel filter
  // (RxBw) locks in. Pick something like 125 kHz here.
  SX1276_WriteReg(REG_AFCBW, 0x0A);
}

// Call this after a successful "real" packet reception to see the
// actual measured frequency offset, in Hz.
int32_t SX1276_ReadAfcOffsetHz(void) {
  uint8_t msb = SX1276_ReadReg(REG_AFCMSB);
  uint8_t lsb = SX1276_ReadReg(REG_AFCLSB);
  int16_t raw = (int16_t)((msb << 8) | lsb); // two's complement
  return (int32_t)raw * 61; // each LSB ≈ FSTEP (61.035 Hz)
}

void SX1276_EnterContinuousRx(void) {
  SX1276_WriteReg(REG_OP_MODE, 0x01); // Standby first
  SleepMsec(1);
  SX1276_EnableAfc();
  SX1276_WriteReg(REG_OP_MODE, 0x05); // FSK, HF band, continuous Receiver mode
  LOG("[SX1276] Listening...\n");
}

// TEMPORARY diagnostic loop - replaces your normal loop() for bench testing.
// No sleep, no reed switches, no TX - just dump whatever the radio hears.
void LoopRx(void) {
  sei();
  SX1276_EnterContinuousRx();
  SX1276_WriteReg(0x12, 0x12); // RegRxBw = 83.3 kHz - matched to your FSK signal

  uint8_t last_sync_state = 0;
  uint32_t last_heartbeat = millis();

  while (1) {
    uint8_t irq1 = SX1276_ReadReg(0x3E); // RegIrqFlags1
    uint8_t irq2 = SX1276_ReadReg(0x3F); // RegIrqFlags2

    // Sync word alone matching is a great early signal, even before
    // a full packet completes - fires much more often than a clean decode.
    uint8_t sync_now = irq1 & 0x01; // SyncAddressMatch
    if (sync_now && !last_sync_state) {
      uint8_t rssi_raw = SX1276_ReadReg(0x11); // RegRssiValue
      LOG("[RX] Sync matched! RSSI=%d dBm\n", -(rssi_raw / 2));
    }
    last_sync_state = sync_now;

    if (irq2 & 0x04) { // PayloadReady
      uint8_t buf[14];
      PORTA.OUTCLR = NSS_PIN;
      SPI_Transfer(REG_FIFO & 0x7F);
      for (uint8_t i = 0; i < 14; i++) buf[i] = SPI_Transfer(0x00);
      PORTA.OUTSET = NSS_PIN;

      const char* classification = buf[0] == 0x51 ? "*** REAL WH51 PACKET ***" : "(noise)";
      uint8_t rssi_raw = SX1276_ReadReg(0x11);
      LOG("[RX] %s RSSI=%d dBm  ID=%02x%02x%02x AFC_offset=%ld Bytes:", classification,
          -(rssi_raw / 2), buf[1], buf[2], buf[3], SX1276_ReadAfcOffsetHz());
      for (uint8_t i = 0; i < 14; i++) LOG(" %02x", buf[i]);
      LOG("\n");

      SX1276_WriteReg(REG_OP_MODE, 0x01);
      SleepMsec(1);
      SX1276_WriteReg(REG_OP_MODE, 0x05);
    }

    // Heartbeat so you know it's alive even with nothing incoming
    if (millis() - last_heartbeat > 10000) {
      uint8_t rssi_raw = SX1276_ReadReg(0x11);
      LOG("[RX] alive, RSSI=%d dBm\n", -(rssi_raw / 2));
      last_heartbeat = millis();
    }
  }
}
#endif

static inline uint8_t ReadReedState(void) {
  uint8_t state = 0;
  if (digitalRead(REED1_PIN) == LOW) state |= 0x01;
  if (digitalRead(REED2_PIN) == LOW) state |= 0x02;
  return state;
}

#define DEBOUNCE_MS 10   // tune to your reed switch's real bounce time

// Blocks (in low-power IDLE sleep) until the reed state has been
// unchanged for DEBOUNCE_MS straight, then returns that settled state.
uint8_t DebounceReedState(void) {
  uint8_t last_sample = ReadReedState();
  uint32_t stable_since = millis();

  while ((millis() - stable_since) < DEBOUNCE_MS) {
    SleepMsec(1);              // low-power 1ms poll tick
    uint8_t sample = ReadReedState();
    if (sample != last_sample) {
      last_sample = sample;
      stable_since = millis();      // any change resets the settle timer
    }
  }
  return last_sample;
}

// --- INTERRUPT SERVICE ROUTINES ---
ISR(PORTA_PORT_vect) {
  // Just wake the CPU - all state logic lives in the main loop.
  PORTA.INTFLAGS = digitalPinToBitMask(REED1_PIN) | digitalPinToBitMask(REED2_PIN);
}

void DeepSleepAllowUdpi() {
  if (!debug_enabled) {
    SLPCTRL.CTRLA = SLPCTRL_SMODE_PDOWN_gc | SLPCTRL_SEN_bm;
    sleep_cpu();
    SLPCTRL.CTRLA &= ~SLPCTRL_SEN_bm;
    return;
  }

  uint16_t timeout = 1000;
  while (!(USART0.STATUS & USART_TXCIF_bm) && --timeout) {
    _delay_us(10);
  }
  USART0.STATUS = USART_TXCIF_bm;

  // --- 2. PRE-SLEEP HARDWARE ISOLATION ---
  // Just release direction - do NOT touch TXEN (errata: leave it enabled)
  PORTB.DIRCLR = DEBUG_TX_PIN;
  PORTB.PIN2CTRL = 0;

  // --- 3. THE DEEP SLEEP EXECUTION ---
  digitalWrite(LED_PIN, HIGH);
  SLPCTRL.CTRLA = SLPCTRL_SMODE_PDOWN_gc | SLPCTRL_SEN_bm;
  sleep_cpu();
  SLPCTRL.CTRLA &= ~SLPCTRL_SEN_bm;
  digitalWrite(LED_PIN, LOW);

  // --- 4. POST-WAKEUP RE-INITIALIZATION & UPDI HALT WINDOW ---
  delay(10);
  PORTB.DIRSET = DEBUG_TX_PIN;   // TXEN was never disabled - nothing else to restore
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  delay(3000);
  digitalWrite(LED_PIN, LOW);
  delay(100);
  pinMode(REED1_PIN, INPUT_PULLUP);
  pinMode(REED2_PIN, INPUT_PULLUP);

  Check_Debug_AutoDetect();

  // Disable input buffers on unused pins to prevent leakage
  //PORTA.PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc; // UPDI - disabling breaks UPDI wake-up sequences.

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

// --- MAIN LOOP ---
void loop(void) {
  // LoopRx();
  sei();
  uint8_t last_reported_state = ReadReedState();

  while (true) {
    if (debug_enabled) _delay_ms(10); // Let UART finish printing

    DeepSleepAllowUdpi();                       // sleeps until any reed edge wakes it
    uint8_t settled_state = DebounceReedState();

    if (settled_state != last_reported_state) {
      LOG("[WAKE] State: %02x\n", settled_state);
      SendPacket(settled_state);
      last_reported_state = settled_state;
    }
  }
}
