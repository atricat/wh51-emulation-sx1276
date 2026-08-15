# wh51-emulation-sx1276
**Emulating a WH51 Soil Moisture Sensor with an ATtiny1614 + SX1276**

This is code to transmit data using an ATtiny1614 MCU and a SX1276 RFM95W module which "fakes" the signals coming from an Ecowitt (Fine Offset) moisture sensor.

**Why would you need this?**

I needed a long-range, low-power (i.e. non 2.4GHz Wifi/Zigbee) battery-powered way to transmit sensor data to Home Assistant.
And I happened to already have WH51 moisture sensors and a gateway set up. These sensors use FSK modulation, and the SX1276 in the gateway can only listen either in FSK mode _or_ simple OOK/ASK mode. So it seemed easier to just quickly set something up to transmit using FSK. How naive... some problems are documented below.


**What works**

- Transmission at 868.35 MHz.
- This shows up on my [LilyGo Lora32](https://lilygo.cc/products/lora3) (essentially an ESP32 and SX1276) running [OpenMQTTGateway](https://docs.openmqttgateway.com/), and thus also on Home Assistant.
- Logging to the same serial port that was used for programming the ATtiny. Logging auto-disabled if no serial detected.
- Reception of actual (genuine Ecowitt) WH51 transmissions, including AFC to figure out the exact frequency. (Needs a tiny bit of code hacking: Call the alternative `LoopRx()` instead of `loop()`.)

**What doesn't work (or is unknown)**

- 433 MHz and 915 MHz (SX1278 board) not implemented, but should be no major problem, see below.
- Transmission is not entirely reliable yet. The repeat burst with 30ms delay may be wrong, TX power may be too high, or the deviation may be off - TBD.
- Unknown whether reception via the official Ecowitt gateway works, I do not own it.

---

## Hardware notes

- MCU: ATtiny1614 (megaTinyCore / Arduino framework)
- Radio: SX1276, SPI, 32.000 MHz crystal (confirm yours matches - the frequency math in the code assumes exactly 32 MHz).
- PA output: configured for **PA_BOOST** (not RFO) - see "SX1276 TX register configuration" below. Most cheap SX1276 breakout boards (RFM95-style especially) only wire the antenna to PA_BOOST; RFO is often not connected at all on the PCB. If you are unsure about your board, first try reception only.
- **Never transmit without an antenna (or dummy load) connected.** An open-circuit PA output
  can degrade or damage the chip over repeated/high-power transmissions.

### Pin numbering

This is specific to the ATtiny1614 - take care to adapt as necessary for other MCUs.
[ATtiny1614/1616/1617 Data Sheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU08/ProductDocuments/DataSheets/ATtiny1614-16-17-DataSheet-DS40002204A.pdf).

![pinout](attiny1614-pinout.png?raw=true)

| Physical pin | Name | Purpose |
|--------------|------|---------|
|       1      | VDD  | 3.3V - not 5V, the SX1276 does not like that |
|       2      | PA4  | SX1276 NSS/CS |
|       3      | PA5  | Reed 1 |
|       4      | PA6  | Reed 2 |
|       5      | PA7  | unused |
|       6      | PB3  | unused |
|       7      | PB2  | Serial output for log messages, see below |
|       8      | PB1  | Status LED connected to GND via ~330 Ohm |
|       9      | PB0  | unused |
|      10      | PA0  | UPDI flashing |
|      11      | PA1  | SX1276 MOSI   |
|      12      | PA2  | SX1276 MISO   |
|      13      | PA3  | SX1276 SCK    |
|      14      | GND  | Heh somehow it's needed |

The unused pins could of course be used for more analog or digital inputs.

I always use the named constants (`PIN_PA1`, `PIN_PB2`, etc.) rather than bare numbers to avoid confusion. This project got bitten by this repeatedly before switching to named constants everywhere.

Related, more general issue that applies beyond just this MCU: Raw AVR/megaAVR-0 registers expect different *kinds* of values depending on which register you're touching, and mixing
them up compiles fine but silently does the wrong thing:
- `DIRSET`/`DIRCLR`/`OUT.../IN`/`INTFLAGS` want a **bitmask** (`PIN1_bm`, or `digitalPinToBitMask(pin)`)
- `PORTx.PINnCTRL` access wants a **bit position** (`digitalPinToBitPosition(pin)`)
- Arduino API calls (`pinMode`, `digitalRead`, `digitalWrite`) want the **Arduino pin number**

This project's ISR originally wrote `PORTA.INTFLAGS = REED1_PIN | REED2_PIN` using Arduino pin *numbers* where a *bitmask* was required - this cleared the wrong bits, leaving the real interrupt flag permanently set, causing an infinite interrupt-retrigger lockup the instant a reed switch changed state. Worth being paranoid about this distinction in any register-level AVR code.

### Serial/UPDI setup

I used a Raspberry Pico with the [Noltari pico-uart-bridge](https://github.com/Noltari/pico-uart-bridge). With a simple setup with two resistors (however took a while to figure out the correct values), one can use the same cable to first program the ATtiny at 230400 baud, then read the output coming back at 9600 baud, e.g. using `minicom -D /dev/ttyACM0 -b 9600` (software/hardware flow control off!).

- Raspi Pico's GPIO16 (pin 21), UART0 TX is connected to a 1 kOhm resistor.
- Raspi Pico's GPIO17 (pin 22), UART0 RX is connected to the ATtiny UPDI (pin 10).
- Raspi Pico's GPIO17 (pin 22), UART0 RX is additionally connected to the other end of the 1 kOhm resistor.
- Another 470 Ohm resistor connects ATtiny UPDI (pin 10) with ATtiny PB2 (pin 7).

Before you ask, I did attempt to use [Philip McGaw's diode based approach](https://philipmcgaw.com/build-a-updi-programmer-from-a-usb-to-uart-adaptor/) because it feels "cleaner" electrically, but couldn't make it work.

If (like this project) you multiplex a debug-UART TX pin so it can also serve as the UPDI programming line while the MCU sleeps, **do not disable/re-enable `USART_TXEN_bm`** around the sleep cycle. Doing so hits a real, reproducible quirk on these parts where the transmit data-register-empty flag gets stuck after TXEN is toggled off and back on, silently swallowing the first print after wake. The pin can be fully isolated for UPDI sharing using only `PORTx.DIR` (input before sleep, output after) - the USART peripheral overrides a pin's output *value* but not its *direction*, so `DIR=input` alone is sufficient isolation, and leaving TXEN permanently enabled the whole time sidesteps the quirk entirely.

---

## SX1276 TX register configuration

```
RegOpMode      (0x01)  Standby = 0x01, TX = 0x03
                        NOTE: bit3 (LowFrequencyModeOn) must be 0 for 868/915 MHz operation.
                        Sequence: Standby -> load FIFO via SPI -> TX. (Triggering TX before
                        the FIFO is loaded, or leaving LowFrequencyModeOn set, are both easy
                        mistakes that silently produce no usable output.)

RegBitrateMsb/Lsb (0x02/0x03)   0x07, 0x40  ->  32MHz / 1856 ~= 17.241 kbit/s

RegFdevMsb/Lsb    (0x04/0x05)   ~30 kHz assumed (0x01, 0xEB) - NOT independently confirmed
                                 against the real sensor's true deviation. See "Open questions"
                                 below - this is the most likely remaining unknown.

RegFrfMsb/Mid/Lsb (0x06/0x07/0x08)  See frequency discussion below - true carrier measured
                                     at ~868.350 MHz, not the commonly-quoted 868.300 MHz.
                                     Frf = round(Freq_Hz * 2^19 / 32,000,000)

RegPaConfig    (0x09)  0x8F  (bit7=1 selects PA_BOOST; OutputPower=15 -> ~17 dBm)

RegFifoThresh  (0x35)  0x8F  (TxStartCondition = FifoNotEmpty)
                        CRITICAL for short payloads: the power-on default requires the FIFO
                        to exceed a 15-byte threshold before TX will even start. The WH51
                        payload is only 14 bytes, so with the default setting TX silently
                        never starts at all (PacketSent never fires) - it's not a timeout,
                        the chip just sits waiting for a FIFO level it will never reach.

RegPreambleMsb/Lsb (0x25/0x26)  0x00, 0x08  (8 bytes of preamble; started at 4, increased for
                                             margin - real receivers often want more preamble
                                             than the theoretical minimum to reliably settle
                                             bit-sync from a cold RX state)

RegSyncConfig  (0x27)  0x11  (SyncOn=1, 2-byte sync word)
RegSyncValue1  (0x28)  0x2D
RegSyncValue2  (0x29)  0xD4

RegPacketConfig1 (0x30)  0x00  (fixed-length packets, no radio-level CRC - CRC is computed
                                 and appended manually as part of the WH51 payload format)
RegPayloadLength (0x32)  14    (NOT 0x38 in terms of register address - see below)
```

### Modulation shaping / PA ramp (`RegPaRamp`, 0x0A)

Left at power-on default: `ModulationShaping` = `00` (none/hard-keyed FSK), bits[3:0] = default
ramp time. Real Fine Offset sensor hardware is presumed (not confirmed) to use simple unshaped
FSK, consistent with this being cost-optimized sensor hardware rather than something using
Gaussian filtering (GFSK). Bit layout for reference: bits[6:5]=ModulationShaping
(00=None/01=GaussianBT1.0/10=GaussianBT0.5/11=GaussianBT0.3), bits[3:0]=ramp time.

---

## The WH51 packet format (reverse-engineered / confirmed against real hardware)

Source of truth: `rtl_433`'s own `fineoffset.c` decoder (the same decoder OMG's `rtl_433_ESP`
library ports), cross-checked and independently verified by capturing real
packets from two genuine WH51 units and recomputing both integrity fields in code - all matched.

14 bytes total, following preamble (`0xAA` repeated) + 2-byte sync (`0x2D 0xD4`):

| Byte(s) | Field |
|---|---|
| 0 | Family code, must be `0x51` |
| 1-3 | Sensor ID (3 bytes, arbitrary/self-assigned) |
| 4 | bits[7:5] = "boost" counter (7 immediately after a change event, decrementing each subsequent TX, 0 at steady state - also affects TX interval: ~10s while boosted, ~70s at steady state); bits[4:0] = battery decivolts (e.g. `0x0F` = 15 = 1500 mV) |
| 5 | Fixed `0x7F` |
| 6 | Moisture %, `0x00`-`0x64` (0-100) |
| 7 | bits[7:1] fixed pattern `1111100`; bit[0] = MSB of 9-bit "AD raw" value |
| 8 | LSB of AD raw value |
| 9-11 | Fixed `0xFF 0xFF 0xFF` |
| 12 | CRC-8 (poly `0x31`, init `0x00`, **non-reflected**/MSB-first) over bytes 0-11 |
| 13 | Additive checksum: `sum(bytes 0-12) mod 256` |

Real-world example (sensor `0f5694`):
```
51 0f 56 94 0f 7f 00 f8 38 ff ff ff 6e 73
```
`0x51`=family,\
`0f 56 94`=ID,\
`0x0f`=boost=0/battery=1500mV,\
`0x7f`=fixed,\
`0x00`=moisture 0%,\
`0xf8`=AD-MSB-bit=0 with fixed pattern,\
`0x38`=AD-LSB (AD raw=56),\
`ff ff ff`=fixed,\
`0x6e`=CRC8(bytes 0-11),\
`0x73`=checksum(bytes 0-12).

Note that the decoder does not validate `moisture` or `ad_raw` against each other or against any expected range - those two fields can be set to whatever you want to represent, such as reed sensors in this case.

---

## Frequency question: 868.300 vs 868.350 MHz

Commonly-quoted configs for EU Fine Offset devices and `rtl_433_ESP`'s own documented build-flag example) use **868.300 MHz**. Using an SX1276 configured for genuine FSK packet-mode reception (not the OOK trick described below) with AFC enabled, and reading back the measured frequency-offset register after real, successful packet captures from two different genuine WH51 units, the true transmit carrier consistently measured ~868.350 MHz.

`Frf` calculation reference:
```
FSTEP = 32,000,000 Hz / 2^19 = 61.03515625 Hz
Frf   = round(Freq_Hz / FSTEP)
```

---

## Retargeting to other ISM bands (433 MHz / 915 MHz)

Everything above assumes the 868 MHz EU band. Two things need to change to move to a different
band: the `RegFrf` value (obviously), and - easy to miss, and the source of a real bug earlier
in this project - the `LowFrequencyModeOn` bit in `RegOpMode`, which selects between the SX1276's
two separate internal PA/synthesizer paths.

### `RegOpMode`: `LowFrequencyModeOn` (bit 3)

| Band | Range | Bit 3 | `RegOpMode` Standby | `RegOpMode` TX |
|---|---|---|---|---|
| ~433 MHz | 410-525 MHz ("low frequency") | `1` | `0x09` | `0x0B` |
| ~868/915 MHz | 862-1020 MHz ("high frequency") | `0` | `0x01` | `0x03` |

### `RegFrf` values

Formula: `Frf = round(Freq_Hz × 2^19 / 32,000,000)`, using a 32 MHz reference crystal (confirm
yours matches).

| Target frequency | `Frf` (decimal) | MSB | MID | LSB |
|---|---|---|---|---|
| 433.920 MHz | 7,109,345 | `0x6C` | `0x7A` | `0xE1` |
| 915.000 MHz | 14,991,360 | `0xE4` | `0xC0` | `0x00` |

(868.350 MHz, this project's own working frequency, is covered above.)

### What does *not* need to change

Bitrate, deviation, preamble length, sync word, `RegPacketConfig1`, and `RegPayloadLength` are
all properties of the *protocol/payload*, not the carrier frequency - they only need to change
if you're also targeting a genuinely different device's protocol, not simply because you moved
bands. If you're specifically trying to spoof the US-market WH51 variant (a 915 MHz variant is
known to exist in the `rtl_433` community, alongside the EU 868 MHz one documented above), the
same packet format documented in this file is very likely still applicable, since it's the same
manufacturer/product family - but don't assume the bitrate/deviation are byte-identical to the
EU variant without checking; treat it with the same "verify against real hardware, don't trust
the first plausible number" skepticism this project needed for 868 MHz. There's no known WH51
variant at 433 MHz - if you're retargeting there, you're almost certainly spoofing a *different*
device's protocol, and the WH51-specific payload format in this document won't apply; only the
radio-level mechanics above (Frf, LowFrequencyModeOn) carry over.

### Hardware caveat: your antenna matching network is fixed per band

Register changes alone are not sufficient - most SX1276 breakout boards have their antenna matching network (and often the antenna itself) physically tuned for one specific band at manufacture time. A board built for 868/915 MHz will radiate poorly, if at all, if you simply reprogram it for 433 MHz - you need a module actually designed for the target band. The registers will happily accept any value and `PacketSent` will still fire normally regardless of whether the antenna is actually resonant at that frequency.

---

## OMG's actual receive architecture

_This section courtesy of Claude who also helped figure out much of the above. But ChatGPT produced a similar theory._

This could be useful for anyone attempting a similar
project against OMG/`rtl_433_ESP` specifically, because it's easy to wrongly assume OMG does
conventional FSK packet-mode reception (matching deviation/bitrate/sync-word registers the way
this project's own RX diagnostic code does) when **it does not**.

Direct review of `rtl_433_ESP`'s source (`rtl_433_ESP.cpp`) shows that for SX1276/SX1278, OMG
configures the radio in **OOK (On-Off Keying) mode**, not FSK packet mode:
```
radio.setDataShapingOOK(2);
radio.setOokThresholdType(RADIOLIB_SX127X_OOK_THRESH_PEAK);
radio.setOokFixedOrFloorThreshold(OokFixedThreshold);  // default 0x0C
```
It uses RadioLib's "direct mode" - the chip outputs a raw demodulated bitstream on a GPIO pin,
which the ESP32 captures via interrupt-timestamped edges, and decodes entirely in **software**
using `rtl_433`'s generic pulse-timing decoders. Architecturally this is the same approach
`rtl_433` uses against a raw SDR dongle - the SX1276 is being used as a crude analog
envelope/threshold detector, not as a packet-engine radio.

This matters because receiving a genuinely FSK-modulated signal via OOK/peak detection is a
known, deliberate trick: tune the receiver's *narrow* filter to sit over only **one** of the two
FSK tones (mark or space, i.e. `carrier ± deviation`), so a frequency shift becomes an amplitude
on/off shift a simple threshold slicer can read as bits. Under this model, OMG's commonly-used
"868.300 MHz" isn't the true carrier at all - it's a deliberately offset tuning point positioned
to catch only one tone, and the ~50 kHz gap to the true measured carrier (868.350 MHz) was
hypothesized to approximate the real transmitter's deviation.

---

## Diagnostic techniques that proved valuable (reusable for similar projects)

How do you figure out which parts of the setup work already, and where we are stuck?

- **SPI: `RegVersion` (0x42) readback, expect `0x12`** - the standard first sanity check that SPI
  wiring/power/chip presence are all fine, before debugging anything else.
- **SPI round-trip test**: write a test byte to an unused scratch register (e.g.
  `RegSyncValue3`, `0x2A`, if you only use a 2-byte sync word) and read it back, to confirm
  writes (not just reads) are reaching the chip.
- **SX1276 `PacketSent` polling (`RegIrqFlags2` bit 3) instead of a blind fixed TX delay** - confirms
  the digital packet engine actually completed, and with a timeout, distinguishes "TX genuinely
  stalled" (e.g. the `RegFifoThresh` bug above) from "TX completed but nothing useful was
  radiated." **Important limitation to remember**: `PacketSent` only proves the *digital*
  side finished - it says nothing about whether real RF power actually left the antenna (that's
  downstream, in the analog PA stage, and can fail silently - e.g. wrong PA_BOOST/RFO pin
  selection - while `PacketSent` still fires normally).
- **A temporary RX-mode build of the same firmware, listening for real reference hardware**,
  was extremely valuable for isolating "is my antenna/RF chain even fundamentally OK" from
  "is my packet content/protocol correct" - two failure categories that look identical from the
  TX side alone (silence either way) but are very different problems. Concretely: put the
  SX1276 in continuous Receiver mode (`RegOpMode=0x05`), poll `RegIrqFlags1` bit0
  (`SyncAddressMatch`) and `RegIrqFlags2` bit2 (`PayloadReady`), dump FIFO bytes + RSSI on each
  hit. **Filter dumps by the expected family byte** (`0x51` here) - raw sync-word matching on
  its own catches a meaningful amount of pure background-noise false positives (statistically
  expected at roughly `listen_time_sec × bitrate / 2^16` for a 16-bit sync word), which can look
  confusingly like "reception" if you don't cross-check packet content.
- **AFC (`RegRxConfig` AfcAutoOn, `RegAfcBw` wider than `RegRxBw`, then read back
  `RegAfcMsb/Lsb`, each LSB ~ FSTEP ~ 61.035 Hz)** is a good way to measure a real transmitter's
  *actual* carrier frequency precisely, once you can receive it at all, rather than trusting a
  commonly-quoted "nominal" frequency that may (as here) turn out to not be the true carrier.

---

## Open questions

1. **OMG's exact effective OOK receive parameters are not fully understood.** Need to analyze OMG's `rtl_433_ESP.cpp` to nail down the real default-path `RegRxBw`
   equivalent, confirm which code branch (OOK vs. FSK) is actually active by default in current
   releases, and re-derive the tone-offset math from there rather than from partial/possibly
   stale code excerpts.
2. **Deviation is still an assumption, not a measured fact**, for the real WH51's TX
   deviation specifically (as opposed to the receive-side tone-offset theory above, which is a
   different, so-far-unconfirmed number). An SDR capture of a real sensor's transmission,
   visually/numerically measuring the actual tone spacing on a spectrogram (e.g. with
   `inspectrum` or Universal Radio Hacker), would settle this definitively and is the natural
   next step.
3. Consider whether matching OMG's specific OOK expectations is worth the effort versus
   alternatives - e.g. a point-to-point link using your own trivial packet format with a
   receiver you also control, if strict compatibility with OMG's existing WH51 decoder isn't a
   hard requirement.
 
