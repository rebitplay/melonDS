#ifndef LAN_WFC_H
#define LAN_WFC_H

#include "../../types.h"

namespace LAN_Wfc
{

bool Init();
void DeInit();

int SendPacket(u8* data, int len);
int RecvPacket(u8* data);

}

#endif
