/*
 *  Copyright (C) 2022       The DOSBox Staging Team
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

#ifndef DOSBOX_MOUSE_INTERNALS_H
#define DOSBOX_MOUSE_INTERNALS_H

#include "mouse.h"
#include "mouse_common.h"

// ***************************************************************************
// Main mouse module
// ***************************************************************************

void MOUSE_Startup();

void MOUSE_NotifyDisconnect(const MouseInterfaceId interface_id);
void MOUSE_NotifyFakePS2(); // fake PS/2 event, for VMware protocol support
void MOUSE_NotifyResetDOS();
void MOUSE_NotifyStateChanged();

// ***************************************************************************
// DOS mouse driver
// ***************************************************************************

void MOUSEDOS_Init();
void MOUSEDOS_NotifyMapped(const bool enabled);
void MOUSEDOS_NotifyRawInput(const bool enabled);
void MOUSEDOS_NotifyMinRate(const uint16_t value_hz,
                            const bool force_update = false);
void MOUSEDOS_DrawCursor();

bool MOUSEDOS_HasCallback(const uint8_t mask);
Bitu MOUSEDOS_DoCallback(const uint8_t mask,
                         const MouseButtons12S buttons_12S);

// - needs relative movements
// - understands up to 3 buttons

bool MOUSEDOS_NotifyMoved(const float x_rel,
                          const float y_rel,
                          const uint16_t x_abs,
                          const uint16_t y_abs);
bool MOUSEDOS_NotifyWheel(const int16_t w_rel);

bool MOUSEDOS_UpdateMoved();
bool MOUSEDOS_UpdateButtons(const MouseButtons12S buttons_12S);
bool MOUSEDOS_UpdateWheel();

// ***************************************************************************
// Serial mouse
// ***************************************************************************

// - needs relative movements
// - understands up to 3 buttons
// - needs index of button which changed state

// Nnothing to declare

// ***************************************************************************
// PS/2 mouse
// ***************************************************************************

void MOUSEPS2_Init();
void MOUSEPS2_UpdateButtonSquish();
void MOUSEPS2_PortWrite(const uint8_t byte);
void MOUSEPS2_UpdatePacket();
bool MOUSEPS2_SendPacket();

// - needs relative movements
// - understands up to 5 buttons in Intellimouse Explorer mode
// - understands up to 3 buttons in other modes
// - provides a way to generate dummy event, for VMware mouse integration

bool MOUSEPS2_NotifyMoved(const float x_rel, const float y_rel);
bool MOUSEPS2_NotifyButton(const MouseButtons12S buttons_12S,
                           const MouseButtonsAll buttons_all);
bool MOUSEPS2_NotifyWheel(const int16_t w_rel);

// ***************************************************************************
// BIOS mouse interface for PS/2 mouse
// ***************************************************************************

Bitu MOUSEBIOS_DoCallback();

// ***************************************************************************
// VMware protocol extension for PS/2 mouse
// ***************************************************************************

void MOUSEVMM_Init();
void MOUSEVMM_NotifyMapped(const bool enabled);
void MOUSEVMM_NotifyRawInput(const bool enabled);
void MOUSEVMM_NewScreenParams(const uint16_t x_abs, const uint16_t y_abs);
void MOUSEVMM_Deactivate();

// - needs absolute mouse position
// - understands up to 3 buttons

bool MOUSEVMM_NotifyMoved(const float x_rel,
                          const float y_rel,
                          const uint16_t x_abs,
                          const uint16_t y_abs);
bool MOUSEVMM_NotifyButton(const MouseButtons12S buttons_12S);
bool MOUSEVMM_NotifyWheel(const int16_t w_rel);


#endif // DOSBOX_MOUSE_INTERNALS_H
