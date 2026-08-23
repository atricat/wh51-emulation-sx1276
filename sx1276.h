#pragma once

void SPI_Init();
bool SX1276_CheckPresence();
void SX1276_Init_FineOffset();
void SX1276_Standby();
void SX1276_SendPacket(uint8_t* payload, uint8_t* payload_end);
bool SX1276_WaitForTxDone(uint16_t timeout_ms);
void SX1276_Sleep();
// Diagnostic loop - replaces your normal loop() for testing.
// No sleep, no reed switches, no TX - just dumps what the radio hears.
void LoopRx();
