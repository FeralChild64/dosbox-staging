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
// Notifications from external subsystems - all should go via these methods
// ***************************************************************************

void  Mouse_EventMoved(int32_t x_rel, int32_t y_rel, int32_t x_abs, int32_t y_abs, bool is_captured);
void  Mouse_EventPressed(uint8_t idx);
void  Mouse_EventReleased(uint8_t idx);
void  Mouse_EventWheel(int32_t w_rel);

void  Mouse_SetSensitivity(int32_t sensitivity_x, int32_t sensitivity_y);
void  Mouse_NewScreenParams(uint16_t clip_x, uint16_t clip_y, uint16_t res_x,  uint16_t res_y,
                            bool fullscreen, int32_t x_abs, int32_t y_abs);

// ***************************************************************************
// Common structures, please only update via functions above
// ***************************************************************************

typedef struct {

    float  sensitivity_x = 0.3f;     // sensitivity, might depend on the GUI/GFX 
    float  sensitivity_y = 0.3f;     // for scaling all relative mouse movements

} MouseInfoConfig;

typedef struct {

    bool     fullscreen = false;
    uint16_t res_x      = 320;       // resolution to which guest image is scaled, excluding black borders
    uint16_t res_y      = 200;
    uint16_t clip_x     = 0;         // clipping = size of black border (one side)
    uint16_t clip_y     = 0;         // clipping value - size of black border (one side)

} MouseInfoVideo;

extern MouseInfoConfig mouse_config;
extern MouseInfoVideo  mouse_video;
extern bool            mouse_vmware; // true = vmware driver took over the mouse

// ***************************************************************************
// Functions to be called by specific interface implementations
// ***************************************************************************

void  Mouse_ClearQueue();
void  Mouse_EventMovedDummy();

// ***************************************************************************
// Serial mouse
// ***************************************************************************

class CSerialMouse;

void  MouseSER_RegisterListener(CSerialMouse *listener);
void  MouseSER_UnRegisterListener(CSerialMouse *listener);

// - needs relative movements
// - understands up to 3 buttons
// - needs index of button which changed state

void  MouseSER_NotifyMoved(int32_t x_rel, int32_t y_rel);
void  MouseSER_NotifyPressed(uint8_t buttons_12S, uint8_t idx);
void  MouseSER_NotifyReleased(uint8_t buttons_12S, uint8_t idx);
void  MouseSER_NotifyWheel(int32_t w_rel);

// ***************************************************************************
// PS/2 mouse
// ***************************************************************************

void  MousePS2_Init();
void  MousePS2_UpdateButtonSquish();
float MousePS2_GetDelay();
void  MousePS2_PortWrite(uint8_t byte);
bool  MousePS2_SendPacket();
uint8_t MousePS2_UpdatePacket();
void  MousePS2_WithDrawPacket();

// - needs relative movements
// - understands up to 5 buttons in Intellimouse Explorer mode
// - understands up to 3 buttons in other modes
// - provides a way to generate dummy event, for VMware mouse integration

void  MousePS2_NotifyMoved(int32_t x_rel, int32_t y_rel);
void  MousePS2_NotifyMovedDummy();
void  MousePS2_NotifyPressedReleased(uint8_t buttons_12S, uint8_t buttons_all);
void  MousePS2_NotifyWheel(int32_t w_rel);

// ***************************************************************************
// BIOS mouse interface for PS/2 mouse
// ***************************************************************************

bool  MouseBIOS_SetState(bool use);
void  MouseBIOS_ChangeCallback(uint16_t pseg, uint16_t pofs);
void  MouseBIOS_Reset();
bool  MouseBIOS_SetPacketSize(uint8_t packet_size);
bool  MouseBIOS_SetRate(uint8_t rate_id);
bool  MouseBIOS_SetResolution(uint8_t res_id);
uint8_t MouseBIOS_GetType();

bool  MouseBIOS_HasCallback();
uintptr_t MouseBIOS_DoCallback();

// ***************************************************************************
// Vmware protocol extension for PS/2 mouse
// ***************************************************************************

void  MouseVMW_Init();
void  MouseVMW_NewScreenParams(int32_t x_abs, int32_t y_abs);

// - needs absolute mouse position
// - understands up to 3 buttons

void  MouseVMW_NotifyMoved(int32_t x_abs, int32_t y_abs);
void  MouseVMW_NotifyPressedReleased(uint8_t buttons_12S);
void  MouseVMW_NotifyWheel(int32_t w_rel);

// ***************************************************************************
// DOS mouse driver
// ***************************************************************************

void  MouseDOS_Init();
void  MouseDOS_BeforeNewVideoMode();
void  MouseDOS_AfterNewVideoMode(bool setmode);
void  MouseDOS_DrawCursor();

bool  MouseDOS_HasCallback(uint8_t type);
bool  MouseDOS_CallbackInProgress();
uintptr_t MouseDOS_DoCallback(uint8_t type, uint8_t buttons);

// - needs relative movements
// - understands up to 3 buttons
// - needs index of button which changed state

void  MouseDOS_NotifyMoved(int32_t x_rel, int32_t y_rel, bool is_captured);
void  MouseDOS_NotifyPressed(uint8_t buttons_12S, uint8_t idx);
void  MouseDOS_NotifyReleased(uint8_t buttons_12S, uint8_t idx);
void  MouseDOS_NotifyWheel(int32_t w_rel);

#endif // DOSBOX_MOUSE_H
