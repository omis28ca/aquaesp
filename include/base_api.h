#pragma once

#include <AqualinkDCodec.h>
#include <stdint.h>

namespace BaseApi {

// Starts WiFi and the HTTP server from a task pinned to core 0.
bool start();

// Called by the core-0 packet consumer. Stores a formatted line for /api/raw.
void recordPacket(const aqualinkd::Packet& packet);

// Safe to call from the RS-485 callback when its handoff queue is full.
void noteDroppedPacket();
uint32_t droppedPackets();

}  // namespace BaseApi
