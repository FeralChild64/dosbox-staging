/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef DOSBOX_MOUSE_H
#define DOSBOX_MOUSE_H

#include "dosbox.h"

class CSerialMouse;

void  MouseSER_Register(CSerialMouse *listener);
void  MouseSER_UnRegister(CSerialMouse *listener);

void  MousePS2_PortWrite(Bit8u byte);

bool  MouseBIOS_SetState(bool use);
void  MouseBIOS_ChangeCallback(Bit16u pseg, Bit16u pofs);

void  MouseBIOS_Reset(void);
bool  MouseBIOS_SetPacketSize(Bit8u packet_size);
void  MouseBIOS_SetRate(Bit8u rate_id);
Bit8u MouseBIOS_GetType(void);

void  MouseDOS_BeforeNewVideoMode();
void  MouseDOS_AfterNewVideoMode(bool setmode);

extern volatile bool mouse_vmware;

void  Mouse_SetSensitivity(Bit32s sensitivity_x, Bit32s sensitivity_y);
void  Mouse_NewScreenParams(Bit16u clip_x, Bit16u clip_y, Bit16u res_x, Bit16u res_y, bool fullscreen, Bit32s x_abs, Bit32s y_abs);

void  Mouse_EventMoved(Bit32s x_rel, Bit32s y_rel, Bit32s x_abs, Bit32s y_abs, bool is_captured);
void  Mouse_EventPressed(Bit8u idx);
void  Mouse_EventReleased(Bit8u idx); 
void  Mouse_EventWheel(Bit32s w_rel);

#endif
