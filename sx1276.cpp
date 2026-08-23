#include <Arduino.h>
#include <avr/io.h>
#include <util/delay.h>

#include "sx1276.h"

#include "config.h"
#include "debug.h"

#define SX1276_FXOSC         32000000UL
#define SX1276_FRF_STEP_DIV  524288UL   // 2^19

// Frf = round(Freq_Hz * 2^19 / FXOSC), computed at compile time
#define SX1276_FRF ((uint32_t)(((uint64_t)RADIO_FREQUENCY_HZ * SX1276_FRF_STEP_DIV \
                                + (SX1276_FXOSC / 2)) / SX1276_FXOSC))

#define SX1276_FRF_MSB ((uint8_t)((SX1276_FRF >> 16) & 0xFF))
#define SX1276_FRF_MID ((uint8_t)((SX1276_FRF >> 8)  & 0xFF))
#define SX1276_FRF_LSB ((uint8_t)( SX1276_FRF        & 0xFF))

// SX1276 registers
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
#define REG_RXCONFIG        0x0D
#define REG_AFCBW           0x13
#define REG_AFCMSB          0x1B
#define REG_AFCLSB          0x1C
#define REG_PREAMBLE_MSB    0x25
#define REG_PREAMBLE_LSB    0x26
#define REG_SYNC_CONFIG     0x27
#define REG_SYNC_VALUE1     0x28
#define REG_SYNC_VALUE2     0x29
#define REG_SYNC_VALUE3     0x2A
#define REG_PACKET_CONFIG1  0x30
#define REG_PAYLOAD_LENGTH  0x32
#define REG_FIFO_THRESH     0x35
#define REG_IRQ_FLAGS2      0x3F
#define REG_VERSION         0x42
#define IRQ2_PACKET_SENT    0x08

void SPI_Init() {
  // Default SPI0 pins - no PORTMUX write needed:
  // MOSI=PA1, MISO=PA2, SCK=PA3, SS=PA4
  PORTA.DIRSET = PIN1_bm | PIN3_bm | NSS_PIN;  // MOSI, SCK, NSS(=PA4) as outputs
  // PA2 (MISO) stays input - that's the power-on default, nothing to set
  PORTA.OUTSET = NSS_PIN;                       // idle high / deselected

  SPI0.CTRLA = SPI_MASTER_bm | SPI_ENABLE_bm | SPI_PRESC_DIV16_gc;
}

uint8_t SPI_Transfer(uint8_t data) {
  SPI0.DATA = data;
  while (!(SPI0.INTFLAGS & SPI_IF_bm));
  return SPI0.DATA;
}

uint8_t SX1276_ReadReg(uint8_t addr) {
  PORTA.OUTCLR = NSS_PIN;
  SPI_Transfer(addr & 0x7F);   // MSB=0 selects read
  uint8_t value = SPI_Transfer(0x00);  // dummy byte to clock out the response
  PORTA.OUTSET = NSS_PIN;
  return value;
}

void SX1276_WriteReg(uint8_t addr, uint8_t value) {
  PORTA.OUTCLR = NSS_PIN;
  SPI_Transfer(addr | 0x80);
  SPI_Transfer(value);
  PORTA.OUTSET = NSS_PIN;
}

bool SX1276_CheckPresence() {
  uint8_t version = SX1276_ReadReg(REG_VERSION);
  LOG("[SX1276] RegVersion = 0x%02x (expect 0x12)\n", version);

  const uint8_t test_pattern = 0xA5;
  SX1276_WriteReg(REG_SYNC_VALUE3, test_pattern);
  uint8_t readback = SX1276_ReadReg(REG_SYNC_VALUE3);
  LOG("[SX1276] R/W test: Wrote 0x%02x, read back 0x%02x\n", test_pattern, readback);

  return version == 0x12 && readback == test_pattern;
}

// --- SX1276 FSK INIT & SLEEP ---
void SX1276_Init_FineOffset() {
  LOG("[SX1276] Init Fine Offset FSK %lu Hz...\n", RADIO_FREQUENCY_HZ);

  SX1276_WriteReg(REG_OP_MODE, 0x00); // FSK Sleep Mode

  SX1276_WriteReg(REG_FIFO_THRESH, 0x80 | 0x0F);  // TxStartCondition = FifoNotEmpty

  // PA_BOOST selected (bit7=1), output power ≈2 + OutputPower dBm (0-15 → 2-17dBm)
  // SX1276_WriteReg(REG_PA_CONFIG, 0x8F); // ~17dBm via PA_BOOST - max for these modules
  // SX1276_WriteReg(REG_PA_CONFIG, 0x8A); // ~12dBm
  // SX1276_WriteReg(REG_PA_CONFIG, 0x85); // ~7dBm
  SX1276_WriteReg(REG_PA_CONFIG, 0x80); // ~2dBm
  // SX1276_WriteReg(REG_PA_CONFIG, 0x80 | RADIO_POWER);

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
  SX1276_WriteReg(REG_FDEV_MSB, 0x02); SX1276_WriteReg(REG_FDEV_LSB, 0x3d); // Deviation = ~35 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x02); SX1276_WriteReg(REG_FDEV_LSB, 0x8F); // Deviation = ~40 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x03); SX1276_WriteReg(REG_FDEV_LSB, 0x23); // Deviation = ~49 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x03); SX1276_WriteReg(REG_FDEV_LSB, 0x85); // Deviation = ~55 kHz
  // SX1276_WriteReg(REG_FDEV_MSB, 0x03); SX1276_WriteReg(REG_FDEV_LSB, 0xA8); // Deviation = ~57 kHz

  // Preamble = 8 Bytes
  SX1276_WriteReg(REG_PREAMBLE_MSB, 0x00);
  SX1276_WriteReg(REG_PREAMBLE_LSB, 0x08);

  // Sync Word = 0x2D 0xD4 (FineOffset Standard)
  SX1276_WriteReg(REG_SYNC_CONFIG, 0x11);
  SX1276_WriteReg(REG_SYNC_VALUE1, 0x2D);
  SX1276_WriteReg(REG_SYNC_VALUE2, 0xD4);

  // Packet Config: Fixed Length, No Radio CRC (we calculate it manually)
  SX1276_WriteReg(REG_PACKET_CONFIG1, 0x00);

  /*static const uint16_t deviation[] = {0x00F6, 0x0148, 0x0168, 0x019A, 0x01C3, 0x01EC, 0x023D, 0x028F, 0x02E1, 0x0333};
    SX1276_WriteReg(REG_FDEV_MSB, deviation[burst%10] >> 8); SX1276_WriteReg(REG_FDEV_LSB, deviation[burst%10] & 0xff);
    SX1276_WriteReg(REG_PREAMBLE_LSB, 0x04*(burst/10));
    #define REG_PA_RAMP 0x0a
    SX1276_WriteReg(REG_PA_RAMP, 0x40);
  */
#define REG_PA_RAMP 0x0a
  // SX1276_WriteReg(REG_PA_RAMP, 0x09); // No shaping
  // SX1276_WriteReg(REG_PA_RAMP, 0x29); // Gaussian BT = 1.0
  // SX1276_WriteReg(REG_PA_RAMP, 0x49); // Gaussian BT = 0.5
  // SX1276_WriteReg(REG_PA_RAMP, 0x69); // Gaussian BT = 0.3
}

void SX1276_Sleep() {
  SX1276_WriteReg(REG_OP_MODE, 0x00); // Deep Sleep (~0.2 uA)
}

// Bitmask to select low-frequency (433 MHz) mode when writing to REG_OP_MODE.
#define OP_MODE_LOW_FREQ (RADIO_FREQUENCY_HZ < 600000000UL ? 0x80 : 0x00)

void SX1276_Standby(){
    // Force the radio into Standby mode to clear FIFO frame pointers.
    SX1276_WriteReg(REG_OP_MODE, 0x01 | OP_MODE_LOW_FREQ); // Standby
}

void SX1276_SendPacket(uint8_t* payload, uint8_t* payload_end){
    uint32_t start = micros();
    SX1276_WriteReg(REG_PAYLOAD_LENGTH, payload_end-payload);
    // Refill the transmission FIFO pipeline.
    PORTA.OUTCLR = NSS_PIN;
    SPI_Transfer(REG_FIFO | 0x80);
    while (payload < payload_end) {
      SPI_Transfer(*payload++);
    }
    PORTA.OUTSET = NSS_PIN;

    // Crystal oscillator wake-up time after SX1276_Standby() is typically 250 µs.
    // It is likely that transfering the payload above will have taken that long, but still check and wait if necessary.
    while (micros() - start < 250);
    
    // Trigger FSK Transmit, switch to TX mode.
    // Gemini argues for 0x0b; "Gaussian Modulation Shaping: Fine Offset transmitters utilize Gaussian filtering"
    SX1276_WriteReg(REG_OP_MODE, 0x03 | OP_MODE_LOW_FREQ); // TX
}

bool SX1276_WaitForTxDone(uint16_t timeout_ms) {
  uint32_t start = millis();
  while (!(SX1276_ReadReg(REG_IRQ_FLAGS2) & IRQ2_PACKET_SENT)) {
    if (millis() - start > timeout_ms) {
      LOG("[SX1276] TX timeout - PacketSent never set!\n");
      return false;
    }
  }
  LOG("[SX1276] PacketSent confirmed after %lums\n", millis() - start);
  return true;
}

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
  return (int32_t)(((int64_t)raw * 61035) / 1000); // 61.035 Hz/LSB (actually 32 MHz/2^19), no overflow
}

void SX1276_EnterContinuousRx(void) {
  SX1276_WriteReg(REG_OP_MODE, 0x01); // Standby first
  delay(1);
  SX1276_EnableAfc();
  SX1276_WriteReg(0x12, 0x12); // RegRxBw = 83.3 kHz - matched to your FSK signal
  SX1276_WriteReg(REG_OP_MODE, 0x05); // FSK, HF band, continuous Receiver mode
  LOG("[SX1276] Listening...\n");
}

// Diagnostic loop - replaces your normal loop() for testing.
// No sleep, no reed switches, no TX - just dump whatever the radio hears.
void LoopRx() {
  sei();
  SX1276_EnterContinuousRx();

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

      const char* classification = buf[0] == 0x51 ? "REAL WH51 PACKET:" : "Noise? ";
      uint8_t rssi_raw = SX1276_ReadReg(0x11);
      LOG("[RX] %s RSSI=%d dBm, ID=%02x%02x%02x, AFC_measured_freq=%ld Hz, data=[", classification,
          -(rssi_raw / 2), buf[1], buf[2], buf[3], RADIO_FREQUENCY_HZ + SX1276_ReadAfcOffsetHz());
      for (uint8_t i = 0; i < 14; i++) LOG(" %02x", buf[i]);
      LOG("]\n");

      SX1276_WriteReg(REG_OP_MODE, 0x01);
      delay(1);
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
