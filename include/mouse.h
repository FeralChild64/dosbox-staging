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

// ***************************************************************************
// Common mouse external notifications
// ***************************************************************************

void  Mouse_EventMoved(Bit32s x_rel, Bit32s y_rel, Bit32s x_abs, Bit32s y_abs, bool is_captured);
void  Mouse_EventMovedDummy();
void  Mouse_EventPressed(Bit8u idx);
void  Mouse_EventReleased(Bit8u idx);
void  Mouse_EventWheel(Bit32s w_rel);

void  Mouse_SetSensitivity(Bit32s sensitivity_x, Bit32s sensitivity_y);
void  Mouse_NewScreenParams(Bit16u clip_x, Bit16u clip_y,
                            Bit16u res_x,  Bit16u res_y,
                            bool fullscreen,
                            Bit32s x_abs,  Bit32s y_abs);

// ***************************************************************************
// Common structures, please only update via functions above
// ***************************************************************************

typedef struct {

    float  sensitivity_x = 0.3f;     // sensitivity, might depend on the GUI/GFX 
    float  sensitivity_y = 0.3f;     // for scaling all relative mouse movements

} MouseInfoConfig;

typedef struct {

    bool   fullscreen = false;
    Bit16u res_x      = 320;         // resolution to which guest image is scaled, excluding black borders
    Bit16u res_y      = 200;
    Bit16u clip_x     = 0;           // clipping = size of black border (one side)
    Bit16u clip_y     = 0;           // clipping value - size of black border (one side)

} MouseInfoVideo;

extern MouseInfoConfig mouse_config;
extern MouseInfoVideo  mouse_video;
extern bool            mouse_vmware; // true = vmware driver took over the mouse

// ***************************************************************************
// Serial mouse
// ***************************************************************************

class CSerialMouse;

void  MouseSER_RegisterListener(CSerialMouse *listener);
void  MouseSER_UnRegisterListener(CSerialMouse *listener);

// - needs relative movements
// - understands up to 3 buttons
// - needs index of button which changed state

void  MouseSER_NotifyMoved(Bit32s x_rel, Bit32s y_rel);
void  MouseSER_NotifyPressed(Bit8u buttons_123, Bit8u idx);
void  MouseSER_NotifyReleased(Bit8u buttons_123, Bit8u idx);
void  MouseSER_NotifyWheel(Bit32s w_rel);

// ***************************************************************************
// PS/2 mouse
// ***************************************************************************

void  MousePS2_UpdateButtonSquish();
void  MousePS2_PortWrite(Bit8u byte);

void  MousePS2_NotifyMovedDummy();

// ***************************************************************************
// BIOS mouse interface for PS/2 mouse
// ***************************************************************************

bool  MouseBIOS_SetState(bool use);
void  MouseBIOS_ChangeCallback(Bit16u pseg, Bit16u pofs);

void  MouseBIOS_Reset(void);
bool  MouseBIOS_SetPacketSize(Bit8u packet_size);
bool  MouseBIOS_SetRate(Bit8u rate_id);
bool  MouseBIOS_SetResolution(Bit8u res_id);
Bit8u MouseBIOS_GetType(void);

// ***************************************************************************
// Vmware protocol extension for PS/2 mouse
// ***************************************************************************

void  MouseVMW_Init();
void  MouseVMW_NewScreenParams(Bit32s x_abs, Bit32s y_abs);

// - needs absolute mouse position
// - understands up to 3 buttons

void  MouseVMW_NotifyMoved(Bit32s x_abs, Bit32s y_abs);
void  MouseVMW_NotifyPressedReleased(Bit8u buttons_123);
void  MouseVMW_NotifyWheel(Bit32s w_rel);

// ***************************************************************************
// DOS mouse driver
// ***************************************************************************

void  MouseDOS_BeforeNewVideoMode();
void  MouseDOS_AfterNewVideoMode(bool setmode);

#endif
