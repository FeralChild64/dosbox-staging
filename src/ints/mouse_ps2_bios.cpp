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

#include "bios.h"
#include "callback.h"
#include "cpu.h"
#include "int10.h"
#include "keyboard.h"
#include "pic.h"
#include "regs.h"


// Here two very closely related mouse interfaces are implemented:
// PS/2 direct hardware access, and the relevant BIOS abstraction layer

// Reference:
// - https://www.digchip.com/datasheets/parts/datasheet/196/HT82M30A-pdf.php
// - https://isdaman.com/alsos/hardware/mouse/ps2interface.htm
// - https://wiki.osdev.org/Mouse_Input

enum PS2_CMD:Bit8u { // commands that can be received from PS/2 port
    NO_COMMAND        = 0x00,
    SET_SCALING_11    = 0xe6,
    SET_SCALING_21    = 0xe7,
    SET_RESOLUTION    = 0xe8,
    GET_STATUS        = 0xe9,
    SET_STREAM_MODE   = 0xea,
    POLL_PACKET       = 0xeb,
    RESET_WRAP_MODE   = 0xec,
    SET_WRAP_MODE     = 0xee,
    SET_REMOTE_MODE   = 0xf0,
    GET_DEV_ID        = 0xf2,
    SET_RATE          = 0xf3,
    ENABLE_DEV        = 0xf4,
    DISABLE_DEV       = 0xf5,
    SET_DEFAULTS      = 0xf6,
    RESET             = 0xff,
};

enum PS2_RES:Bit8u {
    SELF_TEST_PASSED  = 0xaa,
    ACKNOWLEDGE       = 0xfa,
};

enum PS2_TYPE:Bit8u { // mouse type visible via PS/2 interface
    NO_MOUSE          = 0xff,
    STD               = 0x00, // standard 2 or 3 button mouse
    IM                = 0x03, // Microsoft IntelliMouse (3 buttons + wheel)
    XP                = 0x04, // Microsoft IntelliMouse Explorer (5 buttons + wheel)
};

static Bit8u    buttons;                      // currently visible button state
static Bit8u    buttons_all;                  // state of all 5 buttons as visible on the host side
static Bit8u    buttons_12S;                  // state when buttons 3/4/5 act together as button 3 (squished mode)
static float    delta_x, delta_y;             // accumulated mouse movement since last reported
static Bit8s    wheel;                        // NOTE: only fetch this via 'GetResetWheel*' calls!

static PS2_TYPE type;                         // NOTE: only change this via 'SetType' call!
static Bit8u    unlock_idx_im, unlock_idx_xp; // sequence index for unlocking extended protocols

static PS2_CMD  command;                      // command waiting for a parameter
static Bit8u    packet[4];                    // packet to be transferred via hardware port or BIOS interface
static bool     reporting;

static Bit8u    rate_hz;                      // how often (maximum) the mouse event listener can be updated
static float    delay;                        // minimum time between interrupts [ms]
static bool     scaling_21;
static Bit8u    counts_mm;                    // counts per mm
static float    counts_coeff;                 // 1.0 is 4 counts per mm
static bool     modeRemote;                   // true = remote mode, false = stream mode
static bool     modeWrap;                     // true = wrap mode

// ***************************************************************************
// PS/2 interface implementation
// ***************************************************************************

void MousePS2_UpdateButtonSquish() {
    // - if VMware compatible driver is enabled, never try to report
    //   mouse buttons 4 and 5, this would be asking for trouble
    // - for PS/2 modes other than IntelliMouse Explorer there is
    //   no standard way to report buttons 4 and 5
    bool sqush_mode = mouse_vmware || (type != PS2_TYPE::XP);

    buttons = sqush_mode ? buttons_12S : buttons_all;
}

static inline void TerminateUnlock() {
    unlock_idx_im = 0;
    unlock_idx_xp = 0;
}

static void SetType(PS2_TYPE type) {
    TerminateUnlock();

    if (::type != type) {
        ::type = type;
        const char *type_name = nullptr;
        switch (type) {
        case PS2_TYPE::STD:
            type_name = "3 buttons";
            break;
        case PS2_TYPE::IM:
            type_name = "IntelliMouse, wheel, 3 buttons";
            break;
        case PS2_TYPE::XP:
            type_name = "IntelliMouse Explorer, wheel, 5 buttons";
            break;
        default:
            break;
        }

        MousePS2_UpdateButtonSquish();
        packet[0] = 0; // set dummy invalid packet, in case someone tries polling
        packet[1] = 0;
        packet[2] = 0;
        packet[3] = 0;

        LOG_MSG("MOUSE (PS/2): %s", type_name);
    }
}

static inline void AddBuffer(Bit8u byte) {
    KEYBOARD_AddBufferAUX(&byte);
}

static inline Bit8u GetResetWheel4bit() {
    Bit8s tmp = std::clamp(wheel, static_cast<Bit8s>(-0x08), static_cast<Bit8s>(0x07));
    wheel = 0;
    return (tmp >= 0) ? tmp : 0x10 + tmp;
}

static inline Bit8u GetResetWheel8bit() {
    Bit8s tmp = wheel;
    wheel = 0;
    return (tmp >= 0) ? tmp : 0x100 + tmp;
}

static Bit16s ApplyScaling(Bit16s d) {
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

    return static_cast<Bit16s>(std::clamp(2 * d, -0x8000, 0x7fff));
}

static inline void ResetCounters() {
    delta_x = 0.0f;
    delta_y = 0.0f;
    wheel   = 0;
}

Bit8u MousePS2_UpdatePacket() {
    bool packet_needed = false;

    Bit8u  mdat = (buttons & 0x07) | 0x08;
    Bit16s dx   = static_cast<Bit16s>(std::round(delta_x));
    Bit16s dy   = static_cast<Bit16s>(std::round(delta_y));

    delta_x -= dx;
    delta_y -= dy;

    dx = ApplyScaling(dx);
    dy = ApplyScaling(-dy);

    if (dx != 0 || dy != 0)
        packet_needed = true;

    if (type == PS2_TYPE::XP) {
        // There is no overflow for 5-button mouse protocol, see HT82M30A datasheet
        dx = std::clamp(dx, static_cast<Bit16s>(-0xff), static_cast<Bit16s>(0xff));
        dy = std::clamp(dy, static_cast<Bit16s>(-0xff), static_cast<Bit16s>(0xff));       
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

    if (mdat != packet[0])
        packet_needed = true;

    packet[0] = static_cast<Bit8u>(mdat);
    packet[1] = static_cast<Bit8u>(dx % 0x100);
    packet[2] = static_cast<Bit8u>(dy % 0x100);

    Bit8u packet_size = 3;
    if (type == PS2_TYPE::IM) {
        packet[3] = GetResetWheel8bit();
        packet_size = 4;
        if (packet[3] != 0)
            packet_needed = true;
    }
    else if (type == PS2_TYPE::XP) {
        Bit8u old_buttons = packet[3] & 0xf0;
        Bit8u new_buttons = ((buttons & 0x18) << 1);
        packet[3] = GetResetWheel4bit() | new_buttons;
        packet_size = 4;
        if (old_buttons != new_buttons || (packet[3] & 0x0f) != 0)
            packet_needed = true;
    }
    else
        packet[3] = 0;

    return packet_needed ? packet_size : 0;
}

bool MousePS2_SendPacket() {
    Bit8u packet_size = MousePS2_UpdatePacket();

    if (!modeWrap && !modeRemote && reporting && packet_size) {
        return KEYBOARD_AddBufferAUX(&packet[0], packet_size);
    }

    return false;
}

void MousePS2_WithDrawPacket() {
    KEYBOARD_ClrMsgAUX();
}

static void CmdSetResolution(Bit8u counts_mm) {
    TerminateUnlock();
    if (counts_mm != 1 && counts_mm != 2 && counts_mm != 4 && counts_mm != 8)
        counts_mm = 4; // invalid parameter, set default
    
    ::counts_mm    = counts_mm;
    ::counts_coeff = counts_mm / 4.0f;
}

static void CmdSetRate(Bit8u rate_hz) {
    ResetCounters();

    if (rate_hz != 10 && rate_hz != 20 && rate_hz != 40 && rate_hz != 60 && rate_hz != 80 &&
        rate_hz != 100 && rate_hz != 200) {
        TerminateUnlock(); // invalid parameter, set default
        rate_hz = 100;
    }

    ::rate_hz = rate_hz;
    ::delay   = 1000.0 / rate_hz;

    // handle IntelliMouse protocol unlock sequence
    static const std::vector<Bit8u> SEQ_IM = { 200, 100, 80 };
    if (SEQ_IM[unlock_idx_im] != rate_hz)
        unlock_idx_im = 0;
    else if (SEQ_IM.size() == ++unlock_idx_im) {
        SetType(PS2_TYPE::IM);
    }

    // handle IntelliMouse Explorer protocol unlock sequence
    static const std::vector<Bit8u> SEQ_XP = { 200, 200, 80 };
    if (SEQ_XP[unlock_idx_xp] != rate_hz)
        unlock_idx_xp = 0;
    else if (SEQ_XP.size() == ++unlock_idx_xp) {
        SetType(PS2_TYPE::XP);
    }
}

static inline void CmdPollPacket() {
    AddBuffer(packet[0]);
    AddBuffer(packet[1]);
    AddBuffer(packet[2]);
    if (type == PS2_TYPE::IM || type == PS2_TYPE::XP)
        AddBuffer(packet[3]);
}

static void CmdSetDefaults() {
    MousePS2_UpdateButtonSquish();

    rate_hz      = 100;
    delay        = 10.0f;

    counts_mm    = 4;
    counts_coeff = 1.0f;

    scaling_21   = false;
}

static void CmdReset() {

    CmdSetDefaults();
    SetType(PS2_TYPE::STD);

    // Keep button state intact!

    ResetCounters();

    reporting    = true;
    modeRemote   = false;
    modeWrap     = false;
}

static void CmdSetReporting(bool enabled) {
    TerminateUnlock();
    ResetCounters();
    reporting = enabled;
}

static void CmdSetModeRemote(bool enabled) {
    TerminateUnlock();
    modeRemote = enabled;
}

static void CmdSetModeWrap(bool enabled) {
    TerminateUnlock();
    modeWrap = enabled;
}

static void CmdSetScaling(bool enabled) {
    TerminateUnlock();    
    scaling_21 = enabled;
}

float MousePS2_GetDelay() {
    return delay;
}

void MousePS2_PortWrite(Bit8u byte) { // value received from PS/2 port
    if (modeWrap && byte != PS2_CMD::RESET && byte != PS2_CMD::RESET_WRAP_MODE) {
        AddBuffer(byte); // wrap mode, just send bytes back
    } else if (command != PS2_CMD::NO_COMMAND) {
        // value is a parameter for an existing command
        switch (command) {
        case PS2_CMD::SET_RESOLUTION:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetResolution(byte);
            break;
        case PS2_CMD::SET_RATE:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetRate(byte);
            break;
        default:
            LOG_WARNING("Mouse: unimplemented PS/2 command 0x%02x/0x%02x", command, byte);
            break;
        }
        command = PS2_CMD::NO_COMMAND;
    }
    else {
        // value is a new command
        switch (byte) {
        case PS2_CMD::SET_RESOLUTION: // needs additional parameter
        case PS2_CMD::SET_RATE:       // needs additional parameter
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            command = static_cast<PS2_CMD>(byte);
            break;
        case PS2_CMD::POLL_PACKET:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdPollPacket();
            break;
        case PS2_CMD::SET_DEFAULTS:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetDefaults();
            break;
        case PS2_CMD::RESET:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            AddBuffer(PS2_RES::SELF_TEST_PASSED);
            CmdReset();
            AddBuffer(static_cast<Bit8u>(type));
            break;
        case PS2_CMD::GET_DEV_ID:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            AddBuffer(static_cast<Bit8u>(type));
            break;
        case PS2_CMD::ENABLE_DEV:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetReporting(true);
            break;
        case PS2_CMD::DISABLE_DEV:
            AddBuffer(PS2_RES::ACKNOWLEDGE); 
            CmdSetReporting(false);
            break;
        case PS2_CMD::SET_REMOTE_MODE:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetModeRemote(true);
            break;
        case PS2_CMD::RESET_WRAP_MODE:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetModeRemote(false);
            break;
        case PS2_CMD::SET_WRAP_MODE:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetModeWrap(true);
            break;
        case PS2_CMD::SET_STREAM_MODE:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetModeWrap(false);
            break;
        case PS2_CMD::SET_SCALING_21:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetScaling(true);
            break;
        case PS2_CMD::SET_SCALING_11:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            CmdSetScaling(false);
            break;
        case PS2_CMD::GET_STATUS:
            AddBuffer(PS2_RES::ACKNOWLEDGE);
            AddBuffer(((buttons & 1) ? 0x01 : 0x00) | // FIXME: what about remaining bits? Does IntelliMouse use them?
                      ((buttons & 2) ? 0x04 : 0x00) |
                      (scaling_21    ? 0x10 : 0x00) |
                      (reporting     ? 0x20 : 0x00) |
                      (modeRemote    ? 0x40 : 0x00));
            AddBuffer(counts_mm);
            AddBuffer(rate_hz);
            break;
        default:
            LOG_WARNING("Mouse: unimplemented PS/2 command 0x%02x", byte);
            break;
        }
    }
}

void MousePS2_NotifyMoved(Bit32s x_rel, Bit32s y_rel) {
    delta_x += x_rel * mouse_config.sensitivity_x * counts_coeff;
    delta_y += y_rel * mouse_config.sensitivity_y * counts_coeff;
}

void MousePS2_NotifyPressedReleased(Bit8u buttons_12S, Bit8u buttons_all) {
    ::buttons_12S = buttons_12S;
    ::buttons_all = buttons_all;

    MousePS2_UpdateButtonSquish();
}

void MousePS2_NotifyWheel(Bit32s w_rel) {
    wheel = std::clamp(w_rel + wheel, -0x80, 0x7f);
}

// ***************************************************************************
// BIOS interface implementation
// ***************************************************************************

static bool    packet_4bytes = false;

static bool    callback_init = false;
static Bit16u  callback_seg = 0;
static Bit16u  callback_ofs = 0;
static bool    callback_use = false;
static RealPt  ps2_callback = 0;

void MouseBIOS_Reset() {
    CmdReset();
}

bool MouseBIOS_SetState(bool use) {
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

bool MouseBIOS_SetPacketSize(Bit8u packet_size) {
    if (packet_size == 3)
        packet_4bytes = false;
    else if (packet_size == 4)
        packet_4bytes = true;
    else
        return false; // unsupported packet size

    return true;
}

bool MouseBIOS_SetRate(Bit8u rate_id) {
    static const std::vector<Bit8u> CONVTAB = { 10, 20, 40, 60, 80, 100, 200 };
    if (rate_id >= CONVTAB.size())
        return false;

    CmdSetRate(CONVTAB[rate_id]);
    return true;
}

bool MouseBIOS_SetResolution(Bit8u res_id) {
    static const std::vector<Bit8u> CONVTAB = { 1, 2, 4, 8 };
    if (res_id >= CONVTAB.size())
        return false;

    CmdSetResolution(CONVTAB[res_id]); 
    return true;
}

void MouseBIOS_ChangeCallback(Bit16u pseg, Bit16u pofs) {
    if ((pseg == 0) && (pofs == 0)) {
        callback_init = false;
    } else {
        callback_init = true;
        callback_seg  = pseg;
        callback_ofs  = pofs;
    }
}

Bit8u MouseBIOS_GetType() {
    return static_cast<Bit8u>(type);
}

static Bitu MouseBIOS_Callback_ret() {
    CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); // remove 4 words
    return CBRET_NONE;
}

bool MouseBIOS_HasCallback() {
    return callback_use;
}

Bitu MouseBIOS_DoCallback() {
    if (!packet_4bytes) {
        CPU_Push16(packet[0]); 
        CPU_Push16(packet[1]);
        CPU_Push16(packet[2]); 
    } else {
        CPU_Push16((Bit16u) (packet[0] + packet[1] * 0x100));
        CPU_Push16(packet[2]);
        CPU_Push16(packet[3]);
    }
    CPU_Push16((Bit16u) 0);

    CPU_Push16(RealSeg(ps2_callback));
    CPU_Push16(RealOff(ps2_callback));
    SegSet16(cs, callback_seg);
    reg_ip = callback_ofs;

    return CBRET_NONE;
}

void MousePS2_Init() {
    // Callback for ps2 user callback handling
    Bitu call_ps2 = CALLBACK_Allocate();
    CALLBACK_Setup(call_ps2, &MouseBIOS_Callback_ret, CB_RETF, "ps2 bios callback");
    ps2_callback = CALLBACK_RealPointer(call_ps2);

    type = PS2_TYPE::NO_MOUSE;

    MouseBIOS_Reset();
}
