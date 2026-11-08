// What platform.cpp calls back into the Recore frontend for.
#pragma once

#include "types.h"

namespace rebit
{
void FrontendLog(int level, const char* line);
void StoreNDSSave(const melonDS::u8* data, melonDS::u32 length);
void MicOpened(bool open);
int MicRead(melonDS::s16* data, int maxlength);
}
