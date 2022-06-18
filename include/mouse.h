/*
 *  Copyright (C) 2022       The DOSBox Staging Team
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

void MOUSE_EventMoved(const int16_t x_rel, const int16_t y_rel, const uint16_t x_abs, const uint16_t y_abs, const bool is_captured);
void MOUSE_EventPressed(const uint8_t idx);
void MOUSE_EventReleased(const uint8_t idx);
void MOUSE_EventWheel(const int16_t w_rel);

void MOUSE_SetSensitivity(const int32_t sensitivity_x, const int32_t sensitivity_y);
void MOUSE_NewScreenParams(const uint16_t clip_x, const uint16_t clip_y, const uint16_t res_x, const uint16_t res_y,
                           const bool fullscreen, const uint16_t x_abs, const uint16_t y_abs);
void MOUSE_NotifyMovedFake(); // for VMware protocol support

// ***************************************************************************
// Common structures, please only update via functions above
// ***************************************************************************

class MouseInfoConfig {

public:

    float sensitivity_x;     // sensitivity, might depend on the GUI/GFX 
    float sensitivity_y;     // for scaling all relative mouse movements

    MouseInfoConfig();
};

class MouseInfoVideo {

public:

    bool     fullscreen;
    uint16_t res_x;          // resolution to which guest image is scaled, excluding black borders
    uint16_t res_y;
    uint16_t clip_x;         // clipping = size of black border (one side)
    uint16_t clip_y;         // clipping value - size of black border (one side)

    MouseInfoVideo();
};

extern MouseInfoConfig mouse_config;
extern MouseInfoVideo  mouse_video;
extern bool            mouse_vmware; // true = vmware driver took over the mouse

// ***************************************************************************
// Functions to be called by specific interface implementations
// ***************************************************************************

void MOUSE_ClearQueue();
void MOUSE_EventMovedDummy();

// ***************************************************************************
// Serial mouse
// ***************************************************************************

class CSerialMouse;

void MOUSESERIAL_RegisterListener(CSerialMouse &listener);
void MOUSESERIAL_UnRegisterListener(CSerialMouse &listener);

// - needs relative movements
// - understands up to 3 buttons
// - needs index of button which changed state

void MOUSESERIAL_NotifyMoved(const int16_t x_rel, const int16_t y_rel);
void MOUSESERIAL_NotifyPressed(const uint8_t buttons_12S, const uint8_t idx);
void MOUSESERIAL_NotifyReleased(const uint8_t buttons_12S, const uint8_t idx);
void MOUSESERIAL_NotifyWheel(const int16_t w_rel);

// ***************************************************************************
// PS/2 mouse
// ***************************************************************************

void MOUSEPS2AUX_Init();
void MOUSEPS2AUX_UpdateButtonSquish();
float MOUSEPS2AUX_GetDelay();
void MOUSEPS2AUX_PortWrite(const uint8_t byte);
void MOUSEPS2AUX_UpdatePacket();
bool MOUSEPS2AUX_SendPacket();

// - needs relative movements
// - understands up to 5 buttons in Intellimouse Explorer mode
// - understands up to 3 buttons in other modes
// - provides a way to generate dummy event, for VMware mouse integration

bool MOUSEPS2AUX_NotifyMoved(const int16_t x_rel, const int16_t y_rel);
bool MOUSEPS2AUX_NotifyPressedReleased(const uint8_t buttons_12S, const uint8_t buttons_all);
bool MOUSEPS2AUX_NotifyWheel(const int16_t w_rel);

// ***************************************************************************
// BIOS mouse interface for PS/2 mouse
// ***************************************************************************

bool MOUSEBIOS_SetState(const bool use);
void MOUSEBIOS_ChangeCallback(const uint16_t pseg, const uint16_t pofs);
void MOUSEBIOS_Reset();
bool MOUSEBIOS_SetPacketSize(const uint8_t packet_size);
bool MOUSEBIOS_SetRate(const uint8_t rate_id);
bool MOUSEBIOS_SetResolution(const uint8_t res_id);
uint8_t MOUSEBIOS_GetType();

bool  MOUSEBIOS_HasCallback();
uintptr_t MOUSEBIOS_DoCallback();

// ***************************************************************************
// VMware protocol extension for PS/2 mouse
// ***************************************************************************

void MOUSEVMWARE_Init();
void MOUSEVMWARE_NewScreenParams(const uint16_t x_abs, const uint16_t y_abs);

// - needs absolute mouse position
// - understands up to 3 buttons

bool MOUSEVMWARE_NotifyMoved(const uint16_t x_abs, const uint16_t y_abs);
bool MOUSEVMWARE_NotifyPressedReleased(const uint8_t buttons_12S);
bool MOUSEVMWARE_NotifyWheel(const int16_t w_rel);

// ***************************************************************************
// DOS mouse driver
// ***************************************************************************

void MOUSEDOSDRV_Init();
void MOUSEDOSDRV_BeforeNewVideoMode();
void MOUSEDOSDRV_AfterNewVideoMode(const bool setmode);
void MOUSEDOSDRV_DrawCursor();

bool MOUSEDOSDRV_HasCallback();
bool MOUSEDOSDRV_HasCallback(const uint8_t type);
bool MOUSEDOSDRV_CallbackInProgress();
uintptr_t MOUSEDOSDRV_DoCallback(const uint8_t type, const uint8_t buttons);

// - needs relative movements
// - understands up to 3 buttons
// - needs index of button which changed state

bool MOUSEDOSDRV_NotifyMoved(const int16_t x_rel, const int16_t y_rel, const bool is_captured);
bool MOUSEDOSDRV_NotifyPressed(const uint8_t buttons_12S, const uint8_t idx);
bool MOUSEDOSDRV_NotifyReleased(const uint8_t buttons_12S, const uint8_t idx);
bool MOUSEDOSDRV_NotifyWheel(const int16_t w_rel);

#endif // DOSBOX_MOUSE_H
