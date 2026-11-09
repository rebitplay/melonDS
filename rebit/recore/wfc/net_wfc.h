#pragma once
// Rebit WFC for the melonDS 1.x Recore core (net_wfc.cpp)

#include "types.h"

namespace rebit::wfc
{
// Opens the proxy connection (Module.rebitWfcSocketProxy); false when the page gave none.
// SendPacket calls it on the DS's first packet.
bool Init();
void DeInit();
int SendPacket(melonDS::u8* data, int len);
int RecvPacket(melonDS::u8* data);
}
