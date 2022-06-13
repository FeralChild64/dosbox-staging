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
#include "keyboard.h"
#include "pic.h"
#include "regs.h"

CHECK_NARROWING();


// Here two very closely related mouse interfaces are implemented:
// PS/2 direct hardware access, and the relevant BIOS abstraction layer

// Reference:
// - https://www.digchip.com/datasheets/parts/datasheet/196/HT82M30A-pdf.php
// - https://isdaman.com/alsos/hardware/mouse/ps2interface.htm
// - https://wiki.osdev.org/Mouse_Input

enum AuxCommand:uint8_t { // commands that can be received from PS/2 port
    NoCommand     = 0x00,
    SetScaling11  = 0xe6,
    SetScaling21  = 0xe7,
    SetResolution = 0xe8,
    GetStatus     = 0xe9,
    SetStreamMode = 0xea,
    PollPacket    = 0xeb,
    ResetWrapMode = 0xec,
    SetWrapMode   = 0xee,
    SetRemoteMode = 0xf0,
    GetDevId      = 0xf2,
    SetRate       = 0xf3,
    EnableDev     = 0xf4,
    DisableDev    = 0xf5,
    SetDefaults   = 0xf6,
    Reset         = 0xff,
};

enum AuxResponse:uint8_t {
    SelfTestPassed = 0xaa,
    Acknowledge    = 0xfa,
};

enum MouseType:uint8_t { // mouse type visible via PS/2 interface
    NoMouse      = 0xff,
    Standard     = 0x00, // standard 2 or 3 button mouse
    IntelliMouse = 0x03, // Microsoft IntelliMouse (3 buttons + wheel)
    Explorer     = 0x04, // Microsoft IntelliMouse Explorer (5 buttons + wheel)
};

static uint8_t    buttons;                      // currently visible button state
static uint8_t    buttons_all;                  // state of all 5 buttons as visible on the host side
static uint8_t    buttons_12S;                  // state when buttons 3/4/5 act together as button 3 (squished mode)
static float      delta_x, delta_y;             // accumulated mouse movement since last reported
static int8_t     wheel;                        // NOTE: only fetch this via 'GetResetWheel*' calls!

static MouseType  type;                         // NOTE: only change this via 'SetType' call!
static uint8_t    unlock_idx_im, unlock_idx_xp; // sequence index for unlocking extended protocols

static AuxCommand command;                      // command waiting for a parameter
static uint8_t    packet[4];                    // packet to be transferred via hardware port or BIOS interface
static bool       reporting;

static uint8_t    rate_hz;                      // how often (maximum) the mouse event listener can be updated
static float      delay;                        // minimum time between interrupts [ms]
static bool       scaling_21;
static uint8_t    counts_mm;                    // counts per mm
static float      counts_coeff;                 // 1.0 is 4 counts per mm
static bool       modeRemote;                   // true = remote mode, false = stream mode
static bool       modeWrap;                     // true = wrap mode

// ***************************************************************************
// PS/2 interface implementation
// ***************************************************************************

void MOUSEPS2AUX_UpdateButtonSquish() {
    // - if VMware compatible driver is enabled, never try to report
    //   mouse buttons 4 and 5, this would be asking for trouble
    // - for PS/2 modes other than IntelliMouse Explorer there is
    //   no standard way to report buttons 4 and 5
    bool squish_mode = mouse_vmware || (type != MouseType::Explorer);

    buttons = squish_mode ? buttons_12S : buttons_all;
}

static void TerminateUnlock() {
    unlock_idx_im = 0;
    unlock_idx_xp = 0;
}

static void SetType(const MouseType type) {
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

static void AddBuffer(uint8_t byte) {
    KEYBOARD_AddBufferAUX(&byte);
}

static uint8_t GetResetWheel4bit() {
    const int8_t tmp = std::clamp(wheel, static_cast<int8_t>(-0x08), static_cast<int8_t>(0x07));
    wheel = 0;
    return static_cast<uint8_t>((tmp >= 0) ? tmp : 0x10 + tmp);
}

static uint8_t GetResetWheel8bit() {
    const int8_t tmp = wheel;
    wheel = 0;
    return static_cast<uint8_t>((tmp >= 0) ? tmp : 0x100 + tmp);
}

static int16_t ApplyScaling(const int16_t d) {
    if (!scaling_21)
        return d;

    switch (d) {
    case 0:
    case 1:
    case 3:
    case -1:
    case -3: 
        return d;
    case 2:  return 1;
    case 4:  return 6;
    case 5:  return 9;
    case -2: return -1;
    case -4: return -6;
    case -5: return -9;
    default: break;
    }

    return static_cast<int16_t>(std::clamp(2 * d, INT16_MIN, INT16_MAX));
}

static void ResetCounters() {
    delta_x = 0.0f;
    delta_y = 0.0f;
    wheel   = 0;
}

void MOUSEPS2AUX_UpdatePacket() {
    uint8_t mdat = (buttons & 0x07) | 0x08;
    int16_t dx   = static_cast<int16_t>(std::round(delta_x));
    int16_t dy   = static_cast<int16_t>(std::round(delta_y));

    delta_x -= dx;
    delta_y -= dy;

    dx = ApplyScaling(dx);
    dy = ApplyScaling(-dy);

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
    packet[1] = static_cast<uint8_t>(dx % 0x100);
    packet[2] = static_cast<uint8_t>(dy % 0x100);

    if (type == MouseType::IntelliMouse)
        packet[3] = GetResetWheel8bit();
    else if (type == MouseType::Explorer)
        packet[3] = GetResetWheel4bit() | ((buttons & 0x18) << 1);
    else
        packet[3] = 0;
}

bool MOUSEPS2AUX_SendPacket() {
    if (!modeWrap && !modeRemote && reporting)
        return KEYBOARD_AddBufferAUX(&packet[0], (type == MouseType::IntelliMouse || type == MouseType::Explorer) ? 4 : 3);

    return false;
}

void MOUSEPS2AUX_FlushPacket() {
    KEYBOARD_FlushMsgAUX();
}

static void CmdSetResolution(uint8_t counts_mm) {
    TerminateUnlock();
    if (counts_mm != 1 && counts_mm != 2 && counts_mm != 4 && counts_mm != 8)
        counts_mm = 4; // invalid parameter, set default
    
    ::counts_mm    = counts_mm;
    ::counts_coeff = counts_mm / 4.0f;
}

static void CmdSetRate(uint8_t rate_hz) {
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

static void CmdPollPacket() {
    AddBuffer(packet[0]);
    AddBuffer(packet[1]);
    AddBuffer(packet[2]);
    if (type == MouseType::IntelliMouse || type == MouseType::Explorer)
        AddBuffer(packet[3]);
}

static void CmdSetDefaults() {
    MOUSEPS2AUX_UpdateButtonSquish();

    rate_hz      = 100;
    delay        = 10.0f;

    counts_mm    = 4;
    counts_coeff = 1.0f;

    scaling_21   = false;
}

static void CmdReset() {

    CmdSetDefaults();
    SetType(MouseType::Standard);

    // Keep button state intact!

    ResetCounters();

    reporting    = true;
    modeRemote   = false;
    modeWrap     = false;
}

static void CmdSetReporting(const bool enabled) {
    TerminateUnlock();
    ResetCounters();
    reporting = enabled;
}

static void CmdSetModeRemote(const bool enabled) {
    TerminateUnlock();
    modeRemote = enabled;
}

static void CmdSetModeWrap(const bool enabled) {
    TerminateUnlock();
    modeWrap = enabled;
}

static void CmdSetScaling(const bool enabled) {
    TerminateUnlock();    
    scaling_21 = enabled;
}

float MOUSEPS2AUX_GetDelay() {
    return delay;
}

void MOUSEPS2AUX_PortWrite(const uint8_t byte) { // value received from PS/2 port
    if (modeWrap && byte != AuxCommand::Reset && byte != AuxCommand::ResetWrapMode) {
        AddBuffer(byte); // wrap mode, just send bytes back
    } else if (command != AuxCommand::NoCommand) {
        // value is a parameter for an existing command
        switch (command) {
        case AuxCommand::SetResolution:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetResolution(byte);
            break;
        case AuxCommand::SetRate:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetRate(byte);
            break;
        default:
            LOG_WARNING("MOUSE (PS/2): unimplemented PS/2 command 0x%02x/0x%02x", command, byte);
            break;
        }
        command = AuxCommand::NoCommand;
    }
    else {
        // value is a new command
        switch (byte) {
        case AuxCommand::SetResolution: // needs additional parameter
        case AuxCommand::SetRate:       // needs additional parameter
            AddBuffer(AuxResponse::Acknowledge);
            command = static_cast<AuxCommand>(byte);
            break;
        case AuxCommand::PollPacket:
            AddBuffer(AuxResponse::Acknowledge);
            CmdPollPacket();
            break;
        case AuxCommand::SetDefaults:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetDefaults();
            break;
        case AuxCommand::Reset:
            AddBuffer(AuxResponse::Acknowledge);
            AddBuffer(AuxResponse::SelfTestPassed);
            CmdReset();
            AddBuffer(static_cast<uint8_t>(type));
            break;
        case AuxCommand::GetDevId:
            AddBuffer(AuxResponse::Acknowledge);
            AddBuffer(static_cast<uint8_t>(type));
            break;
        case AuxCommand::EnableDev:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetReporting(true);
            break;
        case AuxCommand::DisableDev:
            AddBuffer(AuxResponse::Acknowledge); 
            CmdSetReporting(false);
            break;
        case AuxCommand::SetRemoteMode:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetModeRemote(true);
            break;
        case AuxCommand::ResetWrapMode:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetModeRemote(false);
            break;
        case AuxCommand::SetWrapMode:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetModeWrap(true);
            break;
        case AuxCommand::SetStreamMode:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetModeWrap(false);
            break;
        case AuxCommand::SetScaling21:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetScaling(true);
            break;
        case AuxCommand::SetScaling11:
            AddBuffer(AuxResponse::Acknowledge);
            CmdSetScaling(false);
            break;
        case AuxCommand::GetStatus:
            AddBuffer(AuxResponse::Acknowledge);
            AddBuffer(((buttons & 1) ? 0x01 : 0x00) | // TODO: what about remaining bits? Does IntelliMouse use them?
                      ((buttons & 2) ? 0x04 : 0x00) |
                      (scaling_21    ? 0x10 : 0x00) |
                      (reporting     ? 0x20 : 0x00) |
                      (modeRemote    ? 0x40 : 0x00));
            AddBuffer(counts_mm);
            AddBuffer(rate_hz);
            break;
        default:
            LOG_WARNING("MOUSE (PS/2): unimplemented PS/2 command 0x%02x", byte);
            break;
        }
    }
}

bool MOUSEPS2AUX_NotifyMoved(const int16_t x_rel, const int16_t y_rel) {
    delta_x += static_cast<float>(x_rel) * mouse_config.sensitivity_x * counts_coeff;
    delta_y += static_cast<float>(y_rel) * mouse_config.sensitivity_y * counts_coeff;

    return (delta_x >= 0.5 || delta_x <= -0.5 || delta_y >= 0.5 || delta_y <= -0.5);
}

bool MOUSEPS2AUX_NotifyPressedReleased(const uint8_t buttons_12S, const uint8_t buttons_all) {
    const auto buttons_old = buttons;

    ::buttons_12S = buttons_12S;
    ::buttons_all = buttons_all;
    MOUSEPS2AUX_UpdateButtonSquish();

    return (buttons_old != buttons);
}

bool MOUSEPS2AUX_NotifyWheel(const int16_t w_rel) {
    if (type != MouseType::IntelliMouse && type != MouseType::Explorer) return false;
    wheel = static_cast<int8_t>(std::clamp(w_rel + wheel, INT8_MIN, INT8_MAX));
    return true;
}

// ***************************************************************************
// BIOS interface implementation
// ***************************************************************************

static bool     packet_4bytes = false;

static bool     callback_init = false;
static uint16_t callback_seg = 0;
static uint16_t callback_ofs = 0;
static bool     callback_use = false;
static RealPt   ps2_callback = 0;

void MOUSEBIOS_Reset() {
    CmdReset();
}

bool MOUSEBIOS_SetState(const bool use) {
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

bool MOUSEBIOS_SetPacketSize(const uint8_t packet_size) {
    if (packet_size == 3)
        packet_4bytes = false;
    else if (packet_size == 4)
        packet_4bytes = true;
    else
        return false; // unsupported packet size

    return true;
}

bool MOUSEBIOS_SetRate(const uint8_t rate_id) {
    static const std::vector<uint8_t> CONVTAB = { 10, 20, 40, 60, 80, 100, 200 };
    if (rate_id >= CONVTAB.size())
        return false;

    CmdSetRate(CONVTAB[rate_id]);
    return true;
}

bool MOUSEBIOS_SetResolution(const uint8_t res_id) {
    static const std::vector<uint8_t> CONVTAB = { 1, 2, 4, 8 };
    if (res_id >= CONVTAB.size())
        return false;

    CmdSetResolution(CONVTAB[res_id]); 
    return true;
}

void MOUSEBIOS_ChangeCallback(const uint16_t pseg, const uint16_t pofs) {
    if ((pseg == 0) && (pofs == 0)) {
        callback_init = false;
    } else {
        callback_init = true;
        callback_seg  = pseg;
        callback_ofs  = pofs;
    }
}

uint8_t MOUSEBIOS_GetType() {
    return static_cast<uint8_t>(type);
}

static Bitu MOUSEBIOS_Callback_ret() {
    CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); // remove 4 words
    return CBRET_NONE;
}

bool MOUSEBIOS_HasCallback() {
    return callback_use;
}

Bitu MOUSEBIOS_DoCallback() {
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

void MOUSEPS2AUX_Init() {
    // Callback for ps2 user callback handling
    auto call_ps2 = CALLBACK_Allocate();
    CALLBACK_Setup(call_ps2, &MOUSEBIOS_Callback_ret, CB_RETF, "ps2 bios callback");
    ps2_callback = CALLBACK_RealPointer(call_ps2);

    type = MouseType::NoMouse;

    MOUSEBIOS_Reset();
}
