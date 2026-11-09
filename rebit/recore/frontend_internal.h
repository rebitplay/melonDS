// What platform.cpp calls back into the Recore frontend for.
#pragma once

#include "types.h"

namespace rebit
{
void FrontendLog(int level, const char* line);
void StoreNDSSave(const melonDS::u8* data, melonDS::u32 length);
void MicOpened(bool open);
// melonDS changed its firmware (a game saved Wi-Fi settings or registered a WFC ID)
void StoreFirmware(const melonDS::u8* data, melonDS::u32 length);
int MicRead(melonDS::s16* data, int maxlength);
}
