#pragma once

void SPI_Init();
bool SX1276_CheckPresence();
void SX1276_SendPacket(uint8_t* payload, uint8_t* payload_end);
bool SX1276_WaitForTxDone(uint16_t timeout_ms);
