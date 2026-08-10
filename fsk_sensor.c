// Use "Tools > Clock" setting:
// #ifdef F_CPU
// #  undef F_CPU
// #endif
// #define F_CPU 3333333UL // Default 20MHz / 6 clock on ATtiny1614
// #define F_CPU 20000000UL // Default 20MHz / 6 clock on ATtiny1614

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// --- CONFIGURATION ---
// #define DEVICE_ID_0  0x0F   // 3-byte ID
// #define DEVICE_ID_1  0xDE
// #define DEVICE_ID_2  0xAD
#define REED1_PIN PIN_PA5   // physical pin 3
#define REED2_PIN PIN_PA6   // physical pin 4
#define NSS_PIN   PIN4_bm   // PA4 (physical pin 2) - default SPI0 SS pin, used as manual/software CS
#define LED_PIN PIN_PB1  // physical pin 8
#define DEBUG_TX_PIN        PIN2_bm   // PB2 (Hardware USART0 TX default pin)

// Change just this one line to retarget the frequency.
#define RADIO_FREQUENCY_HZ   868350000UL   // measured via AFC, not assumed
//                           MMMkkk000UL

#define SX1276_FXOSC         32000000UL
#define SX1276_FRF_STEP_DIV  524288UL   // 2^19

// Frf = round(Freq_Hz * 2^19 / FXOSC), computed at compile time
#define SX1276_FRF ((uint32_t)(((uint64_t)RADIO_FREQUENCY_HZ * SX1276_FRF_STEP_DIV \
                                + (SX1276_FXOSC / 2)) / SX1276_FXOSC))

#define SX1276_FRF_MSB  ((uint8_t)((SX1276_FRF >> 16) & 0xFF))
#define SX1276_FRF_MID  ((uint8_t)((SX1276_FRF >> 8)  & 0xFF))
#define SX1276_FRF_LSB  ((uint8_t)( SX1276_FRF         & 0xFF))

// SX1276 FSK Registers
#define REG_FIFO            0x00
#define REG_OP_MODE         0x01
#define REG_BITRATE_MSB     0x02
#define REG_BITRATE_LSB     0x03
#define REG_FDEV_MSB        0x04
#define REG_FDEV_LSB        0x05
#define REG_FRF_MSB         0x06
#define REG_FRF_MID         0x07
#define REG_FRF_LSB         0x08
#define REG_PA_CONFIG       0x09
#define REG_PREAMBLE_MSB    0x25
#define REG_PREAMBLE_LSB    0x26
#define REG_SYNC_CONFIG     0x27
#define REG_SYNC_VALUE1     0x28
#define REG_SYNC_VALUE2     0x29
#define REG_PACKET_CONFIG1  0x30
#define REG_PAYLOAD_LENGTH  0x32
#define REG_FIFO_THRESH     0x35
#define REG_IRQ_FLAGS2      0x3F
#define IRQ2_PACKET_SENT    0x08

#define PINCTRL_OF(p) getPINnCTRLregister(digitalPinToPortStruct(p), digitalPinToBitPosition(p))

// --- GLOBAL DEBUG FLAG ---
bool debug_enabled = false;

// If debug is off, this evaluates instantly to false and skips the printf entirely.
#define LOG(format, ...) do { \
    if (debug_enabled) { \
      printf(format, ##__VA_ARGS__); \
    } \
  } while(0)

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
void Check_Debug_AutoDetect(void) {
  // 1. Leave PB2 as high-impedance input with internal pull-up disabled
  PORTB.DIRCLR = DEBUG_TX_PIN;
  PORTB.PIN2CTRL = 0x00;

  // 2. Allow extra settling time for external USB Serial adapters to power up
  _delay_ms(10);

  // 3. Sample the line state: Serial RX lines sit HIGH when idle
  if (PORTB.IN & DEBUG_TX_PIN) {
    debug_enabled = true;
    UART_Init(9600);

    // Test transmission
    LOG("\n=== ATTINY1614 SENSOR ===\n");
    LOG("USB-serial adapter auto-detected!\n");
  } else {
    debug_enabled = false;
    PORTB.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  }
}

// --- SPI DRIVER ---
void SPI_Init(void) {
  // Default SPI0 pins - no PORTMUX write needed:
  // MOSI=PA1, MISO=PA2, SCK=PA3, SS=PA4
  PORTA.DIRSET = PIN1_bm | PIN3_bm | NSS_PIN;  // MOSI, SCK, NSS(=PA4) as outputs
  // PA2 (MISO) stays input - that's the power-on default, nothing to set
  PORTA.OUTSET = NSS_PIN;                       // idle high / deselected

  SPI0.CTRLA = SPI_MASTER_bm | SPI_ENABLE_bm | SPI_PRESC_DIV16_gc;
}

uint8_t SX1276_ReadReg(uint8_t addr) {
  PORTA.OUTCLR = NSS_PIN;
  SPI_Transfer(addr & 0x7F);   // MSB=0 selects read
  uint8_t value = SPI_Transfer(0x00);  // dummy byte to clock out the response
  PORTA.OUTSET = NSS_PIN;
  return value;
}

bool SX1276_CheckPresence(void) {
  uint8_t version = SX1276_ReadReg(0x42);  // REG_VERSION
  LOG("[SX1276] RegVersion = 0x%02x (expect 0x12)\n", version);

  const uint8_t test_pattern = 0xA5;
  SX1276_WriteReg(0x2A, test_pattern);      // REG_SYNCVALUE3 - unused by you
  uint8_t readback = SX1276_ReadReg(0x2A);
  LOG("[SX1276] R/W test: Wrote 0x%02x, read back 0x%02x\n", test_pattern, readback);

  return version == 0x12 && readback == test_pattern;
}

uint8_t SPI_Transfer(uint8_t data) {
  SPI0.DATA = data;
  while (!(SPI0.INTFLAGS & SPI_IF_bm));
  return SPI0.DATA;
}

void SX1276_WriteReg(uint8_t addr, uint8_t value) {
  PORTA.OUTCLR = NSS_PIN;
  SPI_Transfer(addr | 0x80);
  SPI_Transfer(value);
  PORTA.OUTSET = NSS_PIN;
}

// --- SX1276 FSK INIT & SLEEP ---
void SX1276_Init_FineOffset(void) {
  LOG("[SX1276] Init FineOffset FSK %lu Hz...\n", RADIO_FREQUENCY_HZ);

  SX1276_WriteReg(REG_OP_MODE, 0x00); // FSK Sleep Mode

  SX1276_WriteReg(REG_FIFO_THRESH, 0x80 | 0x0F);  // TxStartCondition = FifoNotEmpty

  // PA_BOOST selected (bit7=1), output power ≈2 + OutputPower dBm (0-15 → 2-17dBm)
  SX1276_WriteReg(REG_PA_CONFIG, 0x8F); // ~17dBm via PA_BOOST — typical for these modules

  // E.g. 868.300 MHz (32MHz oscillator) -> 0xE4C000
  SX1276_WriteReg(REG_FRF_MSB, SX1276_FRF_MSB);
  SX1276_WriteReg(REG_FRF_MID, SX1276_FRF_MID);
  SX1276_WriteReg(REG_FRF_LSB, SX1276_FRF_LSB);

  // FineOffset Bitrate = 17.241 kbps (32e6 / 17241 = 1856 = 0x0740)
  SX1276_WriteReg(REG_BITRATE_MSB, 0x07);
  SX1276_WriteReg(REG_BITRATE_LSB, 0x40);

  // SX1276_WriteReg(REG_FDEV_MSB, 0x01); SX1276_WriteReg(REG_FDEV_LSB, 0x27); // Deviation = ~18 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x01); SX1276_WriteReg(REG_FDEV_LSB, 0x48); // Deviation = ~20 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x01); SX1276_WriteReg(REG_FDEV_LSB, 0x9A); // Deviation = ~25 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x01); SX1276_WriteReg(REG_FDEV_LSB, 0xEB); // Deviation = ~30 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x03); SX1276_WriteReg(REG_FDEV_LSB, 0x23); // Deviation = ~49 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x03); SX1276_WriteReg(REG_FDEV_LSB, 0x85); // Deviation = ~55 kHz
  SX1276_WriteReg(REG_FDEV_MSB, 0x03); SX1276_WriteReg(REG_FDEV_LSB, 0xA8); // Deviation = ~57 kHz

  // Preamble = 8 Bytes
  SX1276_WriteReg(REG_PREAMBLE_MSB, 0x00);
  SX1276_WriteReg(REG_PREAMBLE_LSB, 0x08);

  // Sync Word = 0x2D 0xD4 (FineOffset Standard)
  SX1276_WriteReg(REG_SYNC_CONFIG, 0x11);
  SX1276_WriteReg(REG_SYNC_VALUE1, 0x2D);
  SX1276_WriteReg(REG_SYNC_VALUE2, 0xD4);

  // Packet Config: Fixed Length, No Radio CRC (we calculate it manually)
  SX1276_WriteReg(REG_PACKET_CONFIG1, 0x00);
}

void SX1276_Sleep(void) {
  SX1276_WriteReg(REG_OP_MODE, 0x00); // Deep Sleep (~0.2 uA)
}

// --- CRC8 CALCULATION (FineOffset Poly 0x31) ---
uint8_t crc8(const uint8_t *data, uint8_t len) {
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

bool SX1276_WaitForTxDone(uint16_t timeout_ms) {
  uint32_t start = millis();
  while (!(SX1276_ReadReg(REG_IRQ_FLAGS2) & IRQ2_PACKET_SENT)) {
    if (millis() - start > timeout_ms) {
      LOG("[SX1276] TX timeout - PacketSent never set!\n");
      return false;
    }
  }
  // LOG("[SX1276] PacketSent confirmed after %lums\n", millis() - start);
  return true;
}

// --- BATTERY VOLTAGE MEASUREMENT ---
uint16_t Read_Battery_mV(void) {
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

// Explicitly expose the core's native Arduino delay function to this C file
#ifdef __cplusplus
extern "C" {
#endif
void delay(unsigned long ms);
#ifdef __cplusplus
}
#endif

// Low-power sleep delay helper using IDLE mode
void Sleep_Delay_ms(uint8_t ms) {
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  uint32_t start_time = millis();
  while (millis() - start_time < ms) {
    sleep_cpu(); // Halts CPU clock; background millis timer ISR wakes it every 1ms
  }
  sleep_disable();
}

uint8_t checksum_add(const uint8_t *data, uint8_t len) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < len; i++) sum += data[i];
  return (uint8_t)(sum & 0xFF);
}

// Write the last 3 bytes of the ATtiny1614 factory serial number.
// Result is SERNUM7:SERNUM8:SERNUM9.
static void GetDeviceID(uint8_t* out) {
  #if defined(SIGROW)
  // ATtiny1614: SERNUM7..9 are signature-row offsets 0x0A..0x0C.
  *out++ = SIGROW.SERNUM7;
  *out++ = SIGROW.SERNUM8;
  *out++ = SIGROW.SERNUM9;
  #elif defined(__AVR_ATtiny1614__)
  // ATtiny1614 SIGROW is mapped at 0x1100.
  // SERNUM7/8/9 are offsets 0x0A/0x0B/0x0C.
  volatile const uint8_t *sigrow =
    (volatile const uint8_t *)0x1100;
  *out++  = sigrow[0x0A];
  *out++  = sigrow[0x0B];
  *out++  = sigrow[0x0C];
  #else
#error "Unsupported ATtiny1614 toolchain: no SIGROW access method"
  #endif
}

// --- TRANSMIT FSK PACKET ---
void Send_Packet(uint8_t current_reed_state) {
  uint8_t payload[14];

  payload[0] = 0x51;                      // WH51 family code
  GetDeviceID(payload + 1);
  // payload[1] = DEVICE_ID_0;
  // payload[2] = DEVICE_ID_1;
  // payload[3] = DEVICE_ID_2;

  uint8_t boost = 0;                      // 0-7, real sensors set 7 on a moisture/state change
  // Fake WH51 scales the ATTiny 3.3V supply voltage to 1.5V.
  uint16_t wh51_battery_mV = Read_Battery_mV() * 15 / 33;
  uint8_t battery_code = (uint8_t)((wh51_battery_mV + 50) / 100);
  if (battery_code > 31)
    battery_code = 31;
  payload[4] = (boost << 5) | battery_code;

  payload[5] = 0x7F;                      // fixed

  // Repurpose "moisture" (0-100) to carry your reed state - your call how to map it.
  // Simple example: 0 = both open, 10 = one closed, 30 = both closed.
  payload[6] = current_reed_state * 10;

  uint16_t ad_raw = 56 + 10 * current_reed_state; // no real AD value - just make something up.
  payload[7] = 0xF8 | ((ad_raw >> 8) & 0x01);
  payload[8] = ad_raw & 0xFF;

  payload[9]  = 0xFF;
  payload[10] = 0xFF;
  payload[11] = 0xFF;

  payload[12] = crc8(payload, 12);              // CRC over bytes 0-11
  payload[13] = checksum_add(payload, 13);       // sum of bytes 0-12

  if (debug_enabled) {
    LOG("[TX] Payload:");
    for (int i = 0; i < 14; i++) LOG(" %02x", payload[i]);
    LOG("\n");
  }

  SX1276_WriteReg(REG_PAYLOAD_LENGTH, 14);

  // Dynamic timing tracker: Initial wakeup needs 1ms, second burst loop needs 30ms
  uint16_t standby_duration_ms = 1;

  // --- BURST LOOP: Send multiple times ---
  for (uint8_t burst = 0; burst < 2; burst++) {
    // for (uint8_t burst = 0; burst < 10*4; burst++) {

    // 1. Force the radio into Standby mode to clear FIFO frame pointers
    SX1276_WriteReg(REG_OP_MODE, 0x01); // Standby

    /*static const uint16_t deviation[] = {0x00F6, 0x0148, 0x0168, 0x019A, 0x01C3, 0x01EC, 0x023D, 0x028F, 0x02E1, 0x0333};
      SX1276_WriteReg(REG_FDEV_MSB, deviation[burst%10] >> 8); SX1276_WriteReg(REG_FDEV_LSB, deviation[burst%10] & 0xff);
      SX1276_WriteReg(REG_PREAMBLE_LSB, 0x04*(burst/10));
      #define REG_PA_RAMP 0x0a
      SX1276_WriteReg(REG_PA_RAMP, 0x40);
    */

    // 2. Power-Optimized Gap/Settle Delay (1ms first loop, 30ms second loop)
    Sleep_Delay_ms(standby_duration_ms);

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
    // Sleep_Delay_ms(15);
    SX1276_WaitForTxDone(50);

    // 6. Update configuration values for the next burst pass
    standby_duration_ms = 30;  // Real WH51 uses 30, but OpenMQTTGateway requires 150ms of silence.
  }

  // Final Sequence: Lock down radio back to deep sleep mode
  SX1276_Sleep();
}

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
  Sleep_Delay_ms(1);
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
      Sleep_Delay_ms(1);
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

static inline uint8_t Read_Reed_State(void) {
  uint8_t state = 0;
  if (digitalRead(REED1_PIN) == LOW) state |= 0x01;
  if (digitalRead(REED2_PIN) == LOW) state |= 0x02;
  return state;
}

#define DEBOUNCE_MS 10   // tune to your reed switch's real bounce time

// Blocks (in low-power IDLE sleep) until the reed state has been
// unchanged for DEBOUNCE_MS straight, then returns that settled state.
uint8_t Debounce_Reed_State(void) {
  uint8_t last_sample = Read_Reed_State();
  uint32_t stable_since = millis();

  while ((millis() - stable_since) < DEBOUNCE_MS) {
    Sleep_Delay_ms(1);              // low-power 1ms poll tick
    uint8_t sample = Read_Reed_State();
    if (sample != last_sample) {
      last_sample = sample;
      stable_since = millis();      // any change resets the settle timer
    }
  }
  return last_sample;
}

// --- INTERRUPT SERVICE ROUTINES ---
ISR(PORTA_PORT_vect) {
  // Just wake the CPU - all state logic lives in the main loop now.
  PORTA.INTFLAGS = digitalPinToBitMask(REED1_PIN) | digitalPinToBitMask(REED2_PIN);
}

void SleepCpu_AllowUdpi() {
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

  SPI_Init();
  SX1276_CheckPresence();
  SX1276_Init_FineOffset();
  SX1276_Sleep();
}

// --- MAIN LOOP ---
void loop(void) {
  // LoopRx();
  sei();
  uint8_t last_reported_state = Read_Reed_State();

  while (1) {
    if (debug_enabled) _delay_ms(10); // Let UART finish printing

    SleepCpu_AllowUdpi();                       // sleeps until any reed edge wakes it
    uint8_t settled_state = Debounce_Reed_State();

    if (settled_state != last_reported_state) {
      LOG("[WAKE] State: %02x\n", settled_state);
      Send_Packet(settled_state);
      last_reported_state = settled_state;
    }
  }
}
