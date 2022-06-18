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

#include "mouse.h"

#include <algorithm>

#include "bios.h"
#include "callback.h"
#include "checks.h"
#include "cpu.h"
#include "int10.h"
#include "pic.h"
#include "regs.h"

CHECK_NARROWING();


// Here the BIOS abstraction layer for the PS/2 AUX port mouse is implemented.
// PS/2 direct hardware access is not supported yet.

// Reference:
// - https://www.digchip.com/datasheets/parts/datasheet/196/HT82M30A-pdf.php
// - https://isdaman.com/alsos/hardware/mouse/ps2interface.htm
// - https://wiki.osdev.org/Mouse_Input

enum MouseType:uint8_t { // mouse type visible via PS/2 interface
    NoMouse      = 0xff,
    Standard     = 0x00, // standard 2 or 3 button mouse
    IntelliMouse = 0x03, // Microsoft IntelliMouse (3 buttons + wheel)
    Explorer     = 0x04, // Microsoft IntelliMouse Explorer (5 buttons + wheel)
};

static uint8_t    buttons     = 0;              // currently visible button state
static uint8_t    buttons_all = 0;              // state of all 5 buttons as visible on the host side
static uint8_t    buttons_12S = 0;              // state when buttons 3/4/5 act together as button 3 (squished mode)
static float      delta_x = 0.0f;               // accumulated mouse movement since last reported
static float      delta_y = 0.0f;
static int8_t     wheel   = 0;                  // NOTE: only fetch this via 'GetResetWheel*' calls!

static MouseType  type = MouseType::Standard;   // NOTE: only change this via 'SetType' call!
static uint8_t    unlock_idx_im = 0;            // sequence index for unlocking extended protocol
static uint8_t    unlock_idx_xp = 0;

static uint8_t    packet[4] = {};               // packet to be transferred via BIOS interface

static uint8_t    rate_hz      = 0;             // how often (maximum) the mouse event listener can be updated
static float      delay        = 0.0f;          // minimum time between interrupts [ms]
static uint8_t    counts_mm    = 0.0f;          // counts per mm
static float      counts_coeff = 0.0f;          // 1.0 is 4 counts per mm

// ***************************************************************************
// PS/2 hardware mouse implementation
// ***************************************************************************

void MOUSEPS2AUX_UpdateButtonSquish()
{
    // - if VMware compatible driver is enabled, never try to report
    //   mouse buttons 4 and 5, this would be asking for trouble
    // - for PS/2 modes other than IntelliMouse Explorer there is
    //   no standard way to report buttons 4 and 5
    bool squish_mode = mouse_vmware || (type != MouseType::Explorer);

    buttons = squish_mode ? buttons_12S : buttons_all;
}

static void TerminateUnlock()
{
    unlock_idx_im = 0;
    unlock_idx_xp = 0;
}

static void SetType(const MouseType type)
{
    TerminateUnlock();

    if (::type != type) {
        ::type = type;
        const char *type_name = nullptr;
        switch (type) {
        case MouseType::Standard:
            type_name = "3 buttons";
            break;
        case MouseType::IntelliMouse:
            type_name = "IntelliMouse, wheel, 3 buttons";
            break;
        case MouseType::Explorer:
            type_name = "IntelliMouse Explorer, wheel, 5 buttons";
            break;
        default:
            break;
        }

        MOUSEPS2AUX_UpdateButtonSquish();
        packet[0] = 0; // set dummy invalid packet, in case someone tries polling
        packet[1] = 0;
        packet[2] = 0;
        packet[3] = 0;

        LOG_MSG("MOUSE (PS/2): %s", type_name);
    }
}

static uint8_t GetResetWheel4bit()
{
    const int8_t tmp = std::clamp(wheel, static_cast<int8_t>(-0x08), static_cast<int8_t>(0x07));
    wheel = 0;
    return static_cast<uint8_t>((tmp >= 0) ? tmp : 0x10 + tmp);
}

static uint8_t GetResetWheel8bit()
{
    const int8_t tmp = wheel;
    wheel = 0;
    return static_cast<uint8_t>((tmp >= 0) ? tmp : 0x100 + tmp);
}

static void ResetCounters()
{
    delta_x = 0.0f;
    delta_y = 0.0f;
    wheel   = 0;
}

void MOUSEPS2AUX_UpdatePacket()
{
    uint8_t mdat = (buttons & 0x07) | 0x08;
    int16_t dx   = static_cast<int16_t>(std::round(delta_x));
    int16_t dy   = static_cast<int16_t>(std::round(delta_y));

    delta_x -= dx;
    delta_y -= dy;

    dy = -dy;

    if (type == MouseType::Explorer) {
        // There is no overflow for 5-button mouse protocol, see HT82M30A datasheet
        dx = std::clamp(dx, static_cast<int16_t>(-0xff), static_cast<int16_t>(0xff));
        dy = std::clamp(dy, static_cast<int16_t>(-0xff), static_cast<int16_t>(0xff));       
    } else {
        if ((dx > 0xff) || (dx < -0xff)) mdat |= 0x40; // x overflow
        if ((dy > 0xff) || (dy < -0xff)) mdat |= 0x80; // y overflow
    }

    dx %= 0x100;
    dy %= 0x100;

    if (dx < 0) {
        dx   += 0x100;
        mdat |= 0x10; // sign bit for x
    }
    if (dy < 0) {
        dy   += 0x100;
        mdat |= 0x20; // sign bit for y
    }

    packet[0] = static_cast<uint8_t>(mdat);
    packet[1] = static_cast<uint8_t>(dx);
    packet[2] = static_cast<uint8_t>(dy);

    if (type == MouseType::IntelliMouse)
        packet[3] = GetResetWheel8bit();
    else if (type == MouseType::Explorer)
        packet[3] = GetResetWheel4bit() | ((buttons & 0x18) << 1);
    else
        packet[3] = 0;
}

static void CmdSetResolution(uint8_t counts_mm)
{
    TerminateUnlock();
    if (counts_mm != 1 && counts_mm != 2 && counts_mm != 4 && counts_mm != 8)
        counts_mm = 4; // invalid parameter, set default
    
    ::counts_mm    = counts_mm;
    ::counts_coeff = counts_mm / 4.0f;
}

static void CmdSetRate(uint8_t rate_hz)
{
    ResetCounters();

    if (rate_hz != 10 && rate_hz != 20 && rate_hz != 40 && rate_hz != 60 && rate_hz != 80 &&
        rate_hz != 100 && rate_hz != 200) {
        TerminateUnlock(); // invalid parameter, set default
        rate_hz = 100;
    }

    ::rate_hz = rate_hz;
    ::delay   = 1000.0f / rate_hz;

    // handle IntelliMouse protocol unlock sequence
    static const std::vector<uint8_t> SEQ_IM = { 200, 100, 80 };
    if (SEQ_IM[unlock_idx_im] != rate_hz)
        unlock_idx_im = 0;
    else if (SEQ_IM.size() == ++unlock_idx_im) {
        SetType(MouseType::IntelliMouse);
    }

    // handle IntelliMouse Explorer protocol unlock sequence
    static const std::vector<uint8_t> SEQ_XP = { 200, 200, 80 };
    if (SEQ_XP[unlock_idx_xp] != rate_hz)
        unlock_idx_xp = 0;
    else if (SEQ_XP.size() == ++unlock_idx_xp) {
        SetType(MouseType::Explorer);
    }
}

static void CmdSetDefaults()
{
    MOUSEPS2AUX_UpdateButtonSquish();

    rate_hz      = 100;
    delay        = 10.0f;

    counts_mm    = 4;
    counts_coeff = 1.0f;
}

static void CmdReset()
{
    CmdSetDefaults();
    SetType(MouseType::Standard);
    ResetCounters();
}

float MOUSEPS2AUX_GetDelay()
{
    return delay;
}

bool MOUSEPS2AUX_NotifyMoved(const int16_t x_rel, const int16_t y_rel)
{
    delta_x += static_cast<float>(x_rel) * mouse_config.sensitivity_x * counts_coeff;
    delta_y += static_cast<float>(y_rel) * mouse_config.sensitivity_y * counts_coeff;

    return (delta_x >= 0.5 || delta_x <= -0.5 || delta_y >= 0.5 || delta_y <= -0.5);
}

bool MOUSEPS2AUX_NotifyPressedReleased(const uint8_t buttons_12S, const uint8_t buttons_all)
{
    const auto buttons_old = buttons;

    ::buttons_12S = buttons_12S;
    ::buttons_all = buttons_all;
    MOUSEPS2AUX_UpdateButtonSquish();

    return (buttons_old != buttons);
}

bool MOUSEPS2AUX_NotifyWheel(const int16_t w_rel)
{
    if (type != MouseType::IntelliMouse && type != MouseType::Explorer) return false;
    wheel = static_cast<int8_t>(std::clamp(w_rel + wheel, INT8_MIN, INT8_MAX));
    return true;
}

// ***************************************************************************
// BIOS interface implementation
// ***************************************************************************

// TODO: Once the the physical PS/2 mouse is implemented, BIOS has to be changed
// to interact with I/O ports, not to call PS/2 hardware implementation routines
// directly (no Cmd* calls should be present in BIOS) - otherwise the complicated
// Windows 3.x mouse/keyboard support will get confused. See:
// https://www.os2museum.com/wp/jumpy-ps2-mouse-in-enhanced-mode-windows-3-x/

static bool     packet_4bytes = false;

static bool     callback_init = false;
static uint16_t callback_seg = 0;
static uint16_t callback_ofs = 0;
static bool     callback_use = false;
static RealPt   ps2_callback = 0;

void MOUSEBIOS_Reset()
{
    CmdReset();
}

bool MOUSEBIOS_SetState(const bool use)
{
    if (use && !callback_init) {
        callback_use = false;
        PIC_SetIRQMask(12, true);
        return false;
    } else {
        callback_use = use;
        PIC_SetIRQMask(12, !callback_use);
        return true;
    }
}

bool MOUSEBIOS_SetPacketSize(const uint8_t packet_size)
{
    if (packet_size == 3)
        packet_4bytes = false;
    else if (packet_size == 4)
        packet_4bytes = true;
    else
        return false; // unsupported packet size

    return true;
}

bool MOUSEBIOS_SetRate(const uint8_t rate_id)
{
    static const std::vector<uint8_t> CONVTAB = { 10, 20, 40, 60, 80, 100, 200 };
    if (rate_id >= CONVTAB.size())
        return false;

    CmdSetRate(CONVTAB[rate_id]);
    return true;
}

bool MOUSEBIOS_SetResolution(const uint8_t res_id)
{
    static const std::vector<uint8_t> CONVTAB = { 1, 2, 4, 8 };
    if (res_id >= CONVTAB.size())
        return false;

    CmdSetResolution(CONVTAB[res_id]); 
    return true;
}

void MOUSEBIOS_ChangeCallback(const uint16_t pseg, const uint16_t pofs)
{
    if ((pseg == 0) && (pofs == 0)) {
        callback_init = false;
    } else {
        callback_init = true;
        callback_seg  = pseg;
        callback_ofs  = pofs;
    }
}

uint8_t MOUSEBIOS_GetType()
{
    return static_cast<uint8_t>(type);
}

static Bitu MOUSEBIOS_Callback_ret()
{
    CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); // remove 4 words
    return CBRET_NONE;
}

bool MOUSEBIOS_HasCallback()
{
    return callback_use;
}

Bitu MOUSEBIOS_DoCallback()
{
    if (!packet_4bytes) {
        CPU_Push16(packet[0]); 
        CPU_Push16(packet[1]);
        CPU_Push16(packet[2]); 
    } else {
        CPU_Push16(static_cast<uint16_t>((packet[0] + packet[1] * 0x100)));
        CPU_Push16(packet[2]);
        CPU_Push16(packet[3]);
    }
    CPU_Push16((uint16_t) 0);

    CPU_Push16(RealSeg(ps2_callback));
    CPU_Push16(RealOff(ps2_callback));
    SegSet16(cs, callback_seg);
    reg_ip = callback_ofs;

    return CBRET_NONE;
}

void MOUSEPS2AUX_Init()
{
    // Callback for ps2 user callback handling
    auto call_ps2 = CALLBACK_Allocate();
    CALLBACK_Setup(call_ps2, &MOUSEBIOS_Callback_ret, CB_RETF, "ps2 bios callback");
    ps2_callback = CALLBACK_RealPointer(call_ps2);

    type = MouseType::NoMouse;

    MOUSEBIOS_Reset();
}
