#pragma once
#if defined(ESP32) && defined(RX_CAPTURE)

#include <stddef.h>

class MultiTransportCompanionInterface;

// Drive mode switches off what an RX-capture drive does not need (BLE, the TCP and
// WebSocket companion servers, map tiles, Lua apps and their audio) to free internal
// RAM. The state lives in RAM only: a reboot always comes back with drive mode off.
namespace DriveMode {
void attach(MultiTransportCompanionInterface* si);
bool enter();
void leave();
bool active();
// From a UI callback: enter() or leave() runs on the next loop() pass, not inside
// the callback (they stop and start radio stacks and write files).
void request(bool on);
void loop();
void formatHeap(char* out, size_t cap, const char* phase);
}

#endif
