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

#include <string.h>
#include <math.h>
#include <vector>

#include "callback.h"
#include "mem.h"
#include "regs.h"
#include "cpu.h"
#include "pic.h"
#include "inout.h"
#include "int10.h"
#include "bios.h"
#include "dos_inc.h"
#include "keyboard.h"
#include "video.h"

#include "../hardware/serialport/serialmouse.h"

// Reference:
// - https://www.digchip.com/datasheets/parts/datasheet/196/HT82M30A-pdf.php
// - https://isdaman.com/alsos/hardware/mouse/ps2interface.htm
// - https://www.ctyme.com/intr/int-33.htm
// - https://wiki.osdev.org/Mouse_Input
// - https://wiki.osdev.org/VMware_tools
// - https://wiki.osdev.org/VirtualBox_Guest_Additions (planned support)
// Drivers:
// - https://github.com/NattyNarwhal/vmwmouse
// - https://git.javispedro.com/cgit/vbmouse.git (planned support)


// ***************************************************************************
// PS/2 interface specific definitions
// ***************************************************************************

enum PS2_CMD:Bit8u { // commands that can be received from PS/2 port
    NONE              = 0x00,
    SET_SCALING_11    = 0xe6,
    SET_SCALING_21    = 0xe7,
    SET_RESOLUTION    = 0xe8,
    GET_SCALING       = 0xe9,
    SET_STREAM_MODE   = 0xea,
    POLL_DATA         = 0xeb,
    RESET_WRAP_MODE   = 0xec,
    SET_WRAP_MODE     = 0xee,
    SET_REMOTE_MODE   = 0xf0,
    GET_DEV_ID        = 0xf2,
    SET_SAMPLING_RATE = 0xf3,
    ENABLE_DEV        = 0xf4,
    DISABLE_DEV       = 0xf5,
    SET_DEFAULTS      = 0xf6,
    RESEND            = 0xfe,
    RESET             = 0xff,
};

enum PS2_TYPE:Bit8u { // mouse type visible via PS/2 interface
    STD               = 0x00, // standard 2 or 3 button mouse
    IM                = 0x03, // Microsoft IntelliMouse (3 buttons + wheel)
    XP                = 0x04, // Microsoft IntelliMouse Explorer (5 buttons + wheel)
};

enum PS2_MODE:Bit8u { // mouse mode visible via PS/2 interface
    STREAM, // default one
    REMOTE,
    WRAP
};

// ***************************************************************************
// BIOS interface specific definitions
// ***************************************************************************

#define BIOS_MOUSE_IRQ  12

// ***************************************************************************
// VMware interface specific definitions
// ***************************************************************************

static constexpr io_port_t VMW_PORT   = 0x5658u;        // communication port
static constexpr io_port_t VMW_PORTHB = 0x5659u;        // communication port, high bandwidth

static constexpr Bit32u    VMW_MAGIC  = 0x564D5868u;    // magic number for all VMware calls

enum VMW_CMD:Bit16u {
    GETVERSION         = 10,
    ABSPOINTER_DATA    = 39,
    ABSPOINTER_STATUS  = 40,
    ABSPOINTER_COMMAND = 41,
};

enum VMW_ABSPNT:Bit32u {
    ENABLE             = 0x45414552,
    RELATIVE           = 0xF5,
    ABSOLUTE           = 0x53424152,
};

enum VMW_BUTTON:Bit8u {
    LEFT               = 0x20,
    RIGHT              = 0x10,
    MIDDLE             = 0x08,
};

volatile bool mouse_vmware = false;  // if true, VMware compatible driver has taken over the mouse

// ***************************************************************************
// DOS driver interface specific definitions
// ***************************************************************************

#define DOS_GETPOS_X        (static_cast<Bit16s>(mouse_bios.x) & mouse_dos.gran_x)
#define DOS_GETPOS_Y        (static_cast<Bit16s>(mouse_bios.y) & mouse_dos.gran_y)
#define DOS_X_CURSOR        16
#define DOS_Y_CURSOR        16
#define DOS_X_MICKEY        8
#define DOS_Y_MICKEY        8
#define DOS_HIGHESTBIT      (1 << (DOS_X_CURSOR - 1))
#define DOS_BUTTONS_NUM     3 // FIXME: is there any 5-button mouse software for DOS using
#define DOS_BUTTONS_MASK 0x07 // MOUSE.COM style driver? Until we find some, we are forced 
#define DOS_EVENTS_MASK  0xff // to limit the number of buttons reported

enum DOS_EV:Bit16u {
    MOUSE_MOVED       =   0x01,
    PRESSED_LEFT      =   0x02,
    RELEASED_LEFT     =   0x04,
    PRESSED_RIGHT     =   0x08,
    RELEASED_RIGHT    =   0x10,
    PRESSED_MIDDLE    =   0x20,
    RELEASED_MIDDLE   =   0x40,
    WHEEL_MOVED       =   0x80,
    PRESSED_X1        =  0x100, // FIXME: there seems to be no documentation how to handle
    RELEASED_X1       =  0x200, // 5 buttons in DOS; if a DOS program/driver with 5 button
    PRESSED_X2        =  0x400, // support is found, try to check if INT 0x33 function 0x0C
    RELEASED_X2       =  0x800, // can be somehow used to get notification about extra buttons
};

// ***************************************************************************
// Other definitions
// ***************************************************************************

#define GEN_QUEUE_SIZE  32 // if over 255, increase mouse_gen.events size

// ***************************************************************************
// Internal data
// ***************************************************************************

static struct MouseSerialStruct { // Serial port mouse data

    std::vector<CSerialMouse*> listeners;  // never clean this up manually!

    Bit8u    buttons;
    float    x, y;

    MouseSerialStruct() : // XXX rework - other structures - they should have constructors too
        listeners(),
        buttons(0),
        x(0.0f),
        y(0.0f) {}

} mouse_serial;

static struct { // PS/2 hardware mouse data

    Bit8u    buttons;

    Bit8u    packet_size;
    Bit8u    unlock_idx_im, unlock_idx_xp; // sequence index for unlocking extended protocols
    Bit8u    sampling_rate;                // in Hz
    bool     scaling_21;                   // FIXME: this should affect reported mouse movement
    PS2_TYPE type;
    PS2_CMD  command;                      // command waiting for a parameter
    float    delay;                        // minimum time between interrupts [ms]

    void Init() {
        memset(this, 0, sizeof(*this));
    }

} mouse_ps2;

static struct { // VMware interface mouse data

    Bit8u    buttons;                      // state of mouse buttons, in VMware format
    Bit16u   x, y;                         // mouse position, in VMware/VirtualBox format (scaled from 0 to 0xffff)
    Bit8s    wheel;
    bool     updated;

    Bit16s offset_x, offset_y;             // difference between host and guest mouse x coordinate (in host pixels)

    void Init() {
        memset(this, 0, sizeof(*this));
        x = 0x7fff; // middle of the screen
        y = 0x7fff;
    }

} mouse_vmw;

static struct { // BIOS PS/2 interface mouse data

    Bit16s   wheel;
    float    x, y;
    Bit16s   oldmouseX, oldmouseY;

    bool     callback_init;
    Bit16u   callback_seg, callback_ofs;
    bool     useps2callback;
    Bitu     call_ps2;
    RealPt   ps2_callback;

    void Init() {
        memset(this, 0, sizeof(*this));
    }

} mouse_bios;

static struct { // DOS driver interface mouse data

    bool     enabled;

    float    mickey_x, mickey_y;

    Bit16u   times_pressed[DOS_BUTTONS_NUM];
    Bit16u   times_released[DOS_BUTTONS_NUM];
    Bit16u   last_released_x[DOS_BUTTONS_NUM];
    Bit16u   last_released_y[DOS_BUTTONS_NUM];
    Bit16u   last_pressed_x[DOS_BUTTONS_NUM];
    Bit16u   last_pressed_y[DOS_BUTTONS_NUM];
    Bit16u   last_wheel_moved_x;
    Bit16u   last_wheel_moved_y;

    float    mickeysPerPixel_x, mickeysPerPixel_y;
    float    pixelPerMickey_x,  pixelPerMickey_y;

    Bit16s   min_x, max_x, min_y, max_y;
    Bit16s   gran_x, gran_y;

    Bit16s   updateRegion_x[2];
    Bit16s   updateRegion_y[2];

    Bit16u   doubleSpeedThreshold; // FIXME: this should affect mouse movement

    Bit16u   language;
    Bit8u    page;
    Bit8u    mode;

    // sensitivity
    Bit16u   senv_x_val;
    Bit16u   senv_y_val;
    Bit16u   dspeed_val; // FIXME: this should affect mouse movement
    float    senv_x;
    float    senv_y;

    // mouse cursor
    bool     inhibit_draw;
    Bit16u   hidden, oldhidden;
    bool     background;
    Bit16s   backposx, backposy;
    Bit8u    backData[DOS_X_CURSOR * DOS_Y_CURSOR];
    Bit16u*  screenMask;
    Bit16u*  cursorMask;
    Bit16s   clipx, clipy;
    Bit16s   hotx, hoty;
    Bit16u   textAndMask, textXorMask;
    Bit16u   cursorType;

    // interrupt subroutine
    Bit16u   sub_seg, sub_ofs;
    Bit16u   sub_mask;

    bool     in_UIR;

    void Init() {
        memset(this, 0, sizeof(*this));
        sub_seg = 0x6362; // magic value
        hidden  = 1;      // hide cursor on startup
        mode    = 0xff;   // non-existing mode
    }

} mouse_dos;

static struct { // DOS driver interface mouse data, not to be stored

    Bitu     call_int33, call_int74, int74_ret_callback;
    Bitu     call_mouse_bd;
    Bitu     call_uir;
    RealPt   uir_callback;

    void Init() {
        memset(this, 0, sizeof(*this));
    }

} mouse_nosave;

static struct {

    Bit8u    buttons_extra;                // state of mouse buttons 3 (middle), 4, and 5 as visible on host side

    struct { Bit16u type; Bit8u buttons; } event_queue[GEN_QUEUE_SIZE];
    Bit8u    events;
    bool     timer_in_progress;

    void Init() {
        memset(this, 0, sizeof(*this));
    }

} mouse_gen;

static struct {

    float    sensitivity_x = 0.3f;
    float    sensitivity_y = 0.3f;

} config;

static struct {

    bool     fullscreen;
    Bit16u   res_x, res_y;                 // resolution to which guest image is scaled, excluding black borders
    Bit16u   clip_x, clip_y;               // clipping value - size of black border (one side)

    void Init() {
        memset(this, 0, sizeof(*this));
        this->res_x = 1; // prevent potential division by 0
        this->res_y = 1;
    }

} video;

static void MouseGEN_AddEvent(Bit16u type);
static void MouseGEN_EventHandler(uint32_t);

// ***************************************************************************
// Data - cursor/mask
// ***************************************************************************

static constexpr Bit16u DEFAULT_TEXT_AND_MASK = 0x77FF;
static constexpr Bit16u DEFAULT_TEXT_XOR_MASK = 0x7700;

static Bit16u DEFAULT_SCREEN_MASK[DOS_Y_CURSOR] = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF,
    0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x00FF,
    0x30FF, 0xF87F, 0xF87F, 0xFCFF
};

static Bit16u DEFAULT_CURSOR_MASK[DOS_Y_CURSOR] = {
    0x0000, 0x4000, 0x6000, 0x7000,
    0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600,
    0x0600, 0x0300, 0x0300, 0x0000
};

static Bit16u userdefScreenMask[DOS_Y_CURSOR];
static Bit16u userdefCursorMask[DOS_Y_CURSOR];

// ***************************************************************************
// Serial port interface implementation
// ***************************************************************************

void MouseSERIAL_Register(CSerialMouse *listener) {
    if (listener)
        mouse_serial.listeners.push_back(listener);
}

void MouseSERIAL_UnRegister(CSerialMouse *listener) {
    auto iter = std::find(mouse_serial.listeners.begin(), mouse_serial.listeners.end(), listener);
    if (iter != mouse_serial.listeners.end())
        mouse_serial.listeners.erase(iter);
}

inline void MouseSERIAL_Button(Bit8u button_id) {
    if (button_id < 2) {
        for (auto listener : mouse_serial.listeners)
            listener->onMouseEventButtons(mouse_serial.buttons);
    } else {
        for (auto listener : mouse_serial.listeners)
            listener->onMouseEventButton3(mouse_serial.buttons);
    }
}

inline void MouseSERIAL_WheelMoved(Bit8s wheel) {
    for (auto listener : mouse_serial.listeners)
        listener->onMouseEventWheel(wheel);
}

inline void MouseSERIAL_CursorMoved(Bit32s x_rel, Bit32s y_rel) {
    mouse_serial.x += x_rel * config.sensitivity_x;
    mouse_serial.y += y_rel * config.sensitivity_y;

    Bit16s x_int = static_cast<Bit16s>(mouse_serial.x);
    Bit16s y_int = static_cast<Bit16s>(mouse_serial.y);

    if (x_int != 0 || y_int != 0) {
        for (auto listener : mouse_serial.listeners)
            listener->onMouseEventMoved(x_int, y_int);
        mouse_serial.x = 0.0f;
        mouse_serial.y = 0.0f;
    }
}

// ***************************************************************************
// PS/2 interface implementation
// ***************************************************************************

static void MousePS2_Reset() {

	mouse_ps2.type          = PS2_TYPE::STD;
    mouse_ps2.packet_size   = 3;
    mouse_ps2.sampling_rate = 100;
    mouse_ps2.delay         = 10.0f;

	mouse_ps2.unlock_idx_im = 0;
	mouse_ps2.unlock_idx_xp = 0;
}

static inline void MousePS2_AddBuffer(Bit8u byte) {
    KEYBOARD_AddBuffer(byte | 0x100);
}

static void MousePS2_SendPacket() {
    // XXX provide real implementation, this is a dummy one
    MousePS2_AddBuffer(8); // bit 3 has to be set
    MousePS2_AddBuffer(2); // some movement
    MousePS2_AddBuffer(2);
    MousePS2_AddBuffer(0);
    return;
}

inline bool MousePS2_IsButtonSquishMode() {
    // - if VMware compatible driver is enabled, never try to report mouse buttons 4 and 5, this would be asking for trouble
    // - for PS/2 modes other than IntelliMouse Explorer there is no standard way to report buttons 4 and 5
    return mouse_vmware || (mouse_ps2.type != PS2_TYPE::XP) || (mouse_ps2.packet_size != 4);
}

static void MousePS2_ButtonSquishRefresh() {
    // possibly entering or leaving mouse extra button squish mode
    mouse_ps2.buttons = (mouse_ps2.buttons & 7);
    if (MousePS2_IsButtonSquishMode()) {
        // all extra mouse buttons combines are handled as a middle buttons
        if (mouse_gen.buttons_extra)
            mouse_ps2.buttons |= 8;
    } else {
        // extra mouse buttons are separate from middle button
        mouse_ps2.buttons |= (mouse_gen.buttons_extra << 2);
    }
}

static bool MousePS2_SetPacketSize(Bit8u packet_size) {
    if ((packet_size == 3) ||
        (packet_size == 4 && (mouse_ps2.type == PS2_TYPE::IM || mouse_ps2.type == PS2_TYPE::XP))) {
        mouse_ps2.packet_size = packet_size;

        MousePS2_ButtonSquishRefresh();
        return true;
    }
    return false;
}

static void MousePS2_SetSamplingRate(Bit8u rate_hz) {
    // enforce the minimum allowed value
    mouse_ps2.sampling_rate = rate_hz > 10 ? rate_hz : 10;
    mouse_ps2.delay = 1000.0 / mouse_ps2.sampling_rate;

    // handle IntelliMouse protocol unlock sequence
    static const std::vector<Bit8u> SEQ_IM = { 200, 100, 80 };
    if (SEQ_IM[mouse_ps2.unlock_idx_im]!=mouse_ps2.sampling_rate)
        mouse_ps2.unlock_idx_im = 0;
    else if (SEQ_IM.size() == ++mouse_ps2.unlock_idx_im) {
        mouse_ps2.type = PS2_TYPE::IM;
        mouse_ps2.unlock_idx_im = 0;
        MousePS2_ButtonSquishRefresh();
    }

    // handle IntelliMouse Explorer protocol unlock sequence
    static const std::vector<Bit8u> SEQ_XP = { 200, 200, 80 };
    if (SEQ_XP[mouse_ps2.unlock_idx_xp]!=mouse_ps2.sampling_rate)
        mouse_ps2.unlock_idx_xp = 0;
    else if (SEQ_XP.size() == ++mouse_ps2.unlock_idx_xp) {
        mouse_ps2.type = PS2_TYPE::XP;
        mouse_ps2.unlock_idx_xp = 0;
        MousePS2_ButtonSquishRefresh();
    }
}

void MousePS2_PortWrite(Bit8u byte) { // value received from PS/2 port

    LOG_WARNING("XXX in 0x%02x", byte);

    if (mouse_ps2.command != PS2_CMD::NONE) {
    // value is a parameter for an existing command
    switch (mouse_ps2.command) {
    case PS2_CMD::SET_RESOLUTION:
            // XXX implement
            MousePS2_AddBuffer(0xfa); // acknowledge
            break;
        case PS2_CMD::SET_SAMPLING_RATE:
            MousePS2_SetSamplingRate(byte);
            MousePS2_AddBuffer(0xfa); // acknowledge
            break;
        default:
            LOG_WARNING("Mouse: unimplemented PS/2 command 0x%02x/0x%02x", mouse_ps2.command, byte);
            break;
        }
        mouse_ps2.command = PS2_CMD::NONE;
    }
    else {
        // value is a new command
        switch (byte) {
        case PS2_CMD::SET_RESOLUTION:
        case PS2_CMD::SET_SAMPLING_RATE:
            // commands listed above need additional parameter to be executed
            mouse_ps2.command = static_cast<PS2_CMD>(byte);
        break;
        case PS2_CMD::RESET:
            MousePS2_AddBuffer(0xfa); // acknowledge
            // XXX perform reset
            MousePS2_AddBuffer(0xaa); // self-test passed
            MousePS2_AddBuffer(static_cast<Bit8u>(mouse_ps2.type));
        break;
        case PS2_CMD::GET_DEV_ID:
            MousePS2_AddBuffer(0xfa); // acknowledge
            MousePS2_AddBuffer(static_cast<Bit8u>(mouse_ps2.type));
            break;
        case PS2_CMD::SET_SCALING_11:
            MousePS2_AddBuffer(0xfa); // acknowledge
            mouse_ps2.scaling_21 = false;
            break;
        case PS2_CMD::SET_SCALING_21:
            MousePS2_AddBuffer(0xfa); // acknowledge
            mouse_ps2.scaling_21 = true;
            break;
        case PS2_CMD::GET_SCALING:
        case PS2_CMD::SET_STREAM_MODE:
        case PS2_CMD::POLL_DATA:
        case PS2_CMD::RESET_WRAP_MODE:
        case PS2_CMD::SET_WRAP_MODE:
        case PS2_CMD::SET_REMOTE_MODE:
        case PS2_CMD::ENABLE_DEV:
        case PS2_CMD::DISABLE_DEV:
        case PS2_CMD::SET_DEFAULTS:
        case PS2_CMD::RESEND:
            // XXX
        default:
            LOG_WARNING("Mouse: unimplemented PS/2 command 0x%02x", byte);
            break;
        }
    }
}

// ***************************************************************************
// BIOS interface implementation
// ***************************************************************************

void MouseBIOS_Reset(void) {
	MousePS2_Reset();
}

bool MouseBIOS_SetState(bool use) {
    if (use && (!mouse_bios.callback_init)) {
        mouse_bios.useps2callback = false;
        PIC_SetIRQMask(BIOS_MOUSE_IRQ, true);
        return false;
    }
    mouse_bios.useps2callback = use;
    PIC_SetIRQMask(BIOS_MOUSE_IRQ, !mouse_bios.useps2callback);
    return true;
}

bool MouseBIOS_SetPacketSize(Bit8u packet_size) {
    return MousePS2_SetPacketSize(packet_size);
}

void MouseBIOS_SetSamplingRate(Bit8u rate) {
    static const std::vector<Bit8u> CONVERSION_TABLE = { 10, 20, 40, 60, 80, 100, 200 };
    if (rate >= CONVERSION_TABLE.size())
        MousePS2_SetSamplingRate(0xff);
    else
        MousePS2_SetSamplingRate(CONVERSION_TABLE[rate]);    
}

void MouseBIOS_ChangeCallback(Bit16u pseg, Bit16u pofs) {
    if ((pseg == 0) && (pofs == 0)) {
        mouse_bios.callback_init = false;
    } else {
        mouse_bios.callback_init = true;
        mouse_bios.callback_seg  = pseg;
        mouse_bios.callback_ofs  = pofs;
    }
}

Bit8u MouseBIOS_GetType(void) {
    return static_cast<Bit8u>(mouse_ps2.type);
}

inline Bit8u MouseBIOS_GetResetWheel4bit() {
    // API limits are -8,7 - but let's keep it symmetric
    Bit8s tmp = std::clamp(mouse_bios.wheel, static_cast<Bit16s>(-0x07), static_cast<Bit16s>(0x07));
    mouse_bios.wheel = 0;
    return (tmp >= 0) ? tmp : 0x08 + tmp;
}

inline Bit8u MouseBIOS_GetResetWheel8bit() {
    // API limits are -128,127 - but let's keep it symmetric
    Bit8s tmp = std::clamp(mouse_bios.wheel, static_cast<Bit16s>(-0x7F), static_cast<Bit16s>(0x7F));
    mouse_bios.wheel = 0;
    return (tmp >= 0) ? tmp : 0x100 + tmp;
}

static void MouseBIOS_DoPS2Callback(Bit16u buttons, Bit16s mouseX, Bit16s mouseY) {
    if (mouse_bios.useps2callback) {
        Bit16u mdat  = (buttons & 0x07) | 0x08; // buttons: left, right, middle
        Bit16s xdiff = mouseX - mouse_bios.oldmouseX;
        Bit16s ydiff = mouse_bios.oldmouseY - mouseY;
        mouse_bios.oldmouseX = mouseX;
        mouse_bios.oldmouseY = mouseY;
        if (mouse_ps2.packet_size == 4 && mouse_ps2.type == PS2_TYPE::XP) {
            // There is no overflow for 5-button mouse protocol, see HT82M30A datasheet
            xdiff = std::clamp(xdiff, static_cast<Bit16s>(-0xff), static_cast<Bit16s>(0xff));
            ydiff = std::clamp(ydiff, static_cast<Bit16s>(-0xff), static_cast<Bit16s>(0xff));       
        } else {
            if ((xdiff > 0xff) || (xdiff < -0xff)) mdat |= 0x40; // x overflow
            if ((ydiff > 0xff) || (ydiff < -0xff)) mdat |= 0x80; // y overflow
        }
        xdiff %= 256;
        ydiff %= 256;
        if (xdiff < 0) {
            xdiff += 0x100;
            mdat  |= 0x10;
        }
        if (ydiff < 0) {
            ydiff += 0x100;
            mdat  |= 0x20;
        }

        if (mouse_ps2.packet_size == 4) {
            CPU_Push16((Bit16u) (mdat + (xdiff % 256) * 256));
            CPU_Push16((Bit16u) (ydiff % 256));
            if (mouse_ps2.type == PS2_TYPE::XP) {
                // IntelliMouse Explorer - wheel + 2 extra buttons
                CPU_Push16((Bit16u) MouseBIOS_GetResetWheel4bit() | ((buttons & 0x18) << 1));
            } else {
                // Standard IntelliMouse - wheel
                CPU_Push16((Bit16u) MouseBIOS_GetResetWheel8bit());       
            }
        } else {
            CPU_Push16((Bit16u) mdat); 
            CPU_Push16((Bit16u) (xdiff % 256));
            CPU_Push16((Bit16u) (ydiff % 256)); 
        }
        CPU_Push16((Bit16u) 0);


        CPU_Push16(RealSeg(mouse_bios.ps2_callback));
        CPU_Push16(RealOff(mouse_bios.ps2_callback));
        SegSet16(cs, mouse_bios.callback_seg);
        reg_ip = mouse_bios.callback_ofs;
    }
}

static Bitu MouseBIOS_PS2_DummyHandler(void) {
    CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); CPU_Pop16(); // remove 4 words
    return CBRET_NONE;
}

// ***************************************************************************
// VMware interface implementation
// ***************************************************************************

static inline void MouseVMW_CmdGetVersion() {
    reg_eax = 0; // FIXME: should we respond with something resembling VMware?
    reg_ebx = VMW_MAGIC;
}

static inline void MouseVMW_CmdAbsPointerData() {
    reg_eax = mouse_vmw.buttons;
    reg_ebx = mouse_vmw.x;
    reg_ecx = mouse_vmw.y;
    reg_edx = (mouse_vmw.wheel >= 0) ? mouse_vmw.wheel : 0x100 + mouse_vmw.wheel;

    mouse_vmw.wheel = 0;
}

static inline void MouseVMW_CmdAbsPointerStatus() {
    reg_eax = mouse_vmw.updated ? 4 : 0;
    mouse_vmw.updated = false;
}

static inline void MouseVMW_CmdAbsPointerCommand() {
    switch (reg_ebx) {
    case VMW_ABSPNT::ENABLE:
        break; // can be safely ignored
    case VMW_ABSPNT::RELATIVE:
        mouse_vmware = false;
        MousePS2_ButtonSquishRefresh();
        GFX_UpdateMouseState();
        break;
    case VMW_ABSPNT::ABSOLUTE:
        mouse_vmware = true;
        MousePS2_ButtonSquishRefresh();
        GFX_UpdateMouseState();
        break;
    default:
        LOG_WARNING("Mouse: unimplemented VMware subcommand 0x%08x", reg_ebx);
        break;
    }
}

static Bit16u MouseVMW_PortRead(io_port_t, io_width_t) {
    if (reg_eax != VMW_MAGIC)
        return 0;

    switch (reg_cx) {
    case VMW_CMD::GETVERSION:
        MouseVMW_CmdGetVersion();
        break;
    case VMW_CMD::ABSPOINTER_DATA:
        MouseVMW_CmdAbsPointerData();
        break;
    case VMW_CMD::ABSPOINTER_STATUS:
        MouseVMW_CmdAbsPointerStatus();
        break;
    case VMW_CMD::ABSPOINTER_COMMAND:
        MouseVMW_CmdAbsPointerCommand();
        break;
    default:
        LOG_WARNING("Mouse: unimplemented VMware command 0x%08x", reg_ecx);
        break;
    }

    return reg_ax;
}

static void MouseVMW_CursorMoved(Bit32s x_abs, Bit32s y_abs) {
    float vmw_x, vmw_y;
    if (video.fullscreen) {
        // We have to maintain the diffs (offsets) between host and guest
        // mouse positions; otherwise in case of clipped picture (like
        // 4:3 screen displayed on 16:9 fullscreen mode) we could have
        // an effect of 'sticky' borders if the user moves mouse outside
        // of the guest display area

        if (x_abs + mouse_vmw.offset_x < video.clip_x)
                mouse_vmw.offset_x = video.clip_x - x_abs;
        else if (x_abs + mouse_vmw.offset_x >= video.res_x + video.clip_x)
                mouse_vmw.offset_x = video.res_x + video.clip_x - x_abs - 1;

        if (y_abs + mouse_vmw.offset_y < video.clip_y)
                mouse_vmw.offset_y = video.clip_y - y_abs;
        else if (y_abs + mouse_vmw.offset_y >= video.res_y + video.clip_y)
                mouse_vmw.offset_y = video.res_y + video.clip_y - y_abs - 1;

        vmw_x = x_abs + mouse_vmw.offset_x - video.clip_x;
        vmw_y = y_abs + mouse_vmw.offset_y - video.clip_y;
    }
    else {
        vmw_x = std::max(x_abs - video.clip_x, 0);
        vmw_y = std::max(y_abs - video.clip_y, 0);
    }

    mouse_vmw.x = std::min(0xffffu, static_cast<Bit32u>(vmw_x * 0xffff / (video.res_x - 1) + 0.499));
    mouse_vmw.y = std::min(0xffffu, static_cast<Bit32u>(vmw_y * 0xffff / (video.res_y - 1) + 0.499));

    mouse_vmw.updated = true;
}

// ***************************************************************************
// DOS driver mouse cursor - text mode
// ***************************************************************************

// Write and read directly to the screen. Do no use int_setcursorpos (LOTUS123)
extern void WriteChar(Bit16u col, Bit16u row, Bit8u page, Bit8u chr, Bit8u attr, bool useattr);
extern void ReadCharAttr(Bit16u col, Bit16u row, Bit8u page, Bit16u * result);

void RestoreCursorBackgroundText() {
    if (mouse_dos.hidden || mouse_dos.inhibit_draw) return;

    if (mouse_dos.background) {
        WriteChar(mouse_dos.backposx, mouse_dos.backposy,
        	      real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE),
        	      mouse_dos.backData[0], mouse_dos.backData[1], true);
        mouse_dos.background = false;
    }
}

void DrawCursorText() {    
    // Restore Background
    RestoreCursorBackgroundText();

    // Check if cursor in update region
    if ((DOS_GETPOS_Y <= mouse_dos.updateRegion_y[1]) && (DOS_GETPOS_Y >= mouse_dos.updateRegion_y[0]) &&
        (DOS_GETPOS_X <= mouse_dos.updateRegion_x[1]) && (DOS_GETPOS_X >= mouse_dos.updateRegion_x[0])) {
        return;
    }

    // Save Background
    mouse_dos.backposx = DOS_GETPOS_X >> 3;
    mouse_dos.backposy = DOS_GETPOS_Y >> 3;
    if (mouse_dos.mode < 2) mouse_dos.backposx >>= 1; 

    //use current page (CV program)
    Bit8u page = real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE);
    
    if (mouse_dos.cursorType == 0) {
        Bit16u result;
        ReadCharAttr(mouse_dos.backposx, mouse_dos.backposy, page, &result);
        mouse_dos.backData[0] = (Bit8u) (result & 0xff);
        mouse_dos.backData[1] = (Bit8u) (result >> 8);
        mouse_dos.background  = true;
        // Write Cursor
        result = (result & mouse_dos.textAndMask) ^ mouse_dos.textXorMask;
        WriteChar(mouse_dos.backposx, mouse_dos.backposy, page, (Bit8u) (result & 0xff), (Bit8u) (result >> 8), true);
    } else {
        Bit16u address=page * real_readw(BIOSMEM_SEG,BIOSMEM_PAGE_SIZE);
        address += (mouse_dos.backposy * real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS) + mouse_dos.backposx) * 2;
        address /= 2;
        Bit16u cr = real_readw(BIOSMEM_SEG,BIOSMEM_CRTC_ADDRESS);
        IO_Write(cr    , 0xe);
        IO_Write(cr + 1, (address >> 8) & 0xff);
        IO_Write(cr    , 0xf);
        IO_Write(cr + 1, address & 0xff);
    }
}

// ***************************************************************************
// DOS driver mouse cursor - graphic mode
// ***************************************************************************

static Bit8u gfxReg3CE[9];
static Bit8u index3C4, gfxReg3C5;

static void SaveVgaRegisters() {
    if (IS_VGA_ARCH) {
        for (Bit8u i=0; i<9; i++) {
            IO_Write    (0x3CE,i);
            gfxReg3CE[i] = IO_Read(0x3CF);
        }
        /* Setup some default values in GFX regs that should work */
        IO_Write(0x3CE,3); IO_Write(0x3Cf,0);                //disable rotate and operation
        IO_Write(0x3CE,5); IO_Write(0x3Cf,gfxReg3CE[5]&0xf0);    //Force read/write mode 0

        //Set Map to all planes. Celtic Tales
        index3C4 = IO_Read(0x3c4);  IO_Write(0x3C4,2);
        gfxReg3C5 = IO_Read(0x3C5); IO_Write(0x3C5,0xF);
    } else if (machine==MCH_EGA) {
        //Set Map to all planes.
        IO_Write(0x3C4,2);
        IO_Write(0x3C5,0xF);
    }
}

static void RestoreVgaRegisters() {
    if (IS_VGA_ARCH) {
        for (Bit8u i=0; i<9; i++) {
            IO_Write(0x3CE,i);
            IO_Write(0x3CF,gfxReg3CE[i]);
        }

        IO_Write(0x3C4,2);
        IO_Write(0x3C5,gfxReg3C5);
        IO_Write(0x3C4,index3C4);
    }
}

static void ClipCursorArea(Bit16s& x1, Bit16s& x2, Bit16s& y1, Bit16s& y2,
                           Bit16u& addx1, Bit16u& addx2, Bit16u& addy) {
    addx1 = addx2 = addy = 0;
    // Clip up
    if (y1 < 0) {
        addy += (-y1);
        y1 = 0;
    }
    // Clip down
    if (y2 > mouse_dos.clipy) {
        y2 = mouse_dos.clipy;        
    };
    // Clip left
    if (x1 < 0) {
        addx1 += (-x1);
        x1 = 0;
    };
    // Clip right
    if (x2 > mouse_dos.clipx) {
        addx2 = x2 - mouse_dos.clipx;
        x2 = mouse_dos.clipx;
    };
}

static void RestoreCursorBackground() {
    if (mouse_dos.hidden || mouse_dos.inhibit_draw) return;

    SaveVgaRegisters();
    if (mouse_dos.background) {
        // Restore background
        Bit16s x, y;
        Bit16u addx1, addx2, addy;
        Bit16u dataPos = 0;
        Bit16s x1      = mouse_dos.backposx;
        Bit16s y1      = mouse_dos.backposy;
        Bit16s x2      = x1 + DOS_X_CURSOR - 1;
        Bit16s y2      = y1 + DOS_Y_CURSOR - 1;    

        ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

        dataPos = addy * DOS_X_CURSOR;
        for (y = y1; y <= y2; y++) {
            dataPos += addx1;
            for (x = x1; x <= x2; x++) {
                INT10_PutPixel(x, y, mouse_dos.page, mouse_dos.backData[dataPos++]);
            };
            dataPos += addx2;
        };
        mouse_dos.background = false;
    };
    RestoreVgaRegisters();
}

static void DrawCursor() {
    if (mouse_dos.hidden || mouse_dos.inhibit_draw) return;
    INT10_SetCurMode();
    // In Textmode ?
    if (CurMode->type == M_TEXT) {
        DrawCursorText();
        return;
    }

    // Check video page. Seems to be ignored for text mode. 
    // hence the text mode handled above this
    // >>> removed because BIOS page is not actual page in some cases, e.g. QQP games
//    if (real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE)!=mouse_dos.page) return;

// Check if cursor in update region
/*    if ((DOS_GETPOS_X >= mouse_dos.updateRegion_x[0]) && (DOS_GETPOS_X <= mouse_dos.updateRegion_x[1]) &&
        (DOS_GETPOS_Y >= mouse_dos.updateRegion_y[0]) && (DOS_GETPOS_Y <= mouse_dos.updateRegion_y[1])) {
        if (CurMode->type==M_TEXT16)
            RestoreCursorBackgroundText();
        else
            RestoreCursorBackground();
        mouse.shown--;
        return;
    }
   */ /*Not sure yet what to do update region should be set to ??? */
         
    // Get Clipping ranges


    mouse_dos.clipx = (Bit16s) ((Bits) CurMode->swidth  - 1);    /* Get from bios ? */
    mouse_dos.clipy = (Bit16s) ((Bits) CurMode->sheight - 1);

    /* might be vidmode == 0x13?2:1 */
    Bit16s xratio = 640;
    if (CurMode->swidth>0) xratio/=CurMode->swidth;
    if (xratio==0) xratio = 1;
    
    RestoreCursorBackground();

    SaveVgaRegisters();

    // Save Background
    Bit16s x, y;
    Bit16u addx1, addx2, addy;
    Bit16u dataPos   = 0;
    Bit16s x1        = DOS_GETPOS_X / xratio - mouse_dos.hotx;
    Bit16s y1        = DOS_GETPOS_Y - mouse_dos.hoty;
    Bit16s x2        = x1 + DOS_X_CURSOR - 1;
    Bit16s y2        = y1 + DOS_Y_CURSOR - 1;    

    ClipCursorArea(x1,x2,y1,y2, addx1, addx2, addy);

    dataPos = addy * DOS_X_CURSOR;
    for (y = y1; y <= y2; y++) {
        dataPos += addx1;
        for (x = x1; x <= x2; x++) {
            INT10_GetPixel(x, y, mouse_dos.page, &mouse_dos.backData[dataPos++]);
        };
        dataPos += addx2;
    };
    mouse_dos.background = true;
    mouse_dos.backposx   = DOS_GETPOS_X / xratio - mouse_dos.hotx;
    mouse_dos.backposy   = DOS_GETPOS_Y - mouse_dos.hoty;

    // Draw Mousecursor
    dataPos = addy * DOS_X_CURSOR;
    for (y = y1; y <= y2; y++) {
        Bit16u scMask = mouse_dos.screenMask[addy + y - y1];
        Bit16u cuMask = mouse_dos.cursorMask[addy + y - y1];
        if (addx1 > 0) { scMask <<= addx1; cuMask <<= addx1; dataPos += addx1; };
        for (x = x1; x <= x2; x++) {
            Bit8u pixel = 0;
            // ScreenMask
            if (scMask & DOS_HIGHESTBIT) pixel = mouse_dos.backData[dataPos];
            scMask<<=1;
            // CursorMask
            if (cuMask & DOS_HIGHESTBIT) pixel = pixel ^ 0x0f;
            cuMask<<=1;
            // Set Pixel
            INT10_PutPixel(x, y, mouse_dos.page, pixel);
            dataPos++;
        };
        dataPos += addx2;
    };
    RestoreVgaRegisters();
}

// ***************************************************************************
// DOS driver interface implementation
// ***************************************************************************

static inline Bit8u MouseDOS_GetResetWheel16bit(void) {
    Bit16s tmp = (mouse_bios.wheel >= 0) ? mouse_bios.wheel : 0x10000 + mouse_bios.wheel;
    mouse_bios.wheel = 0;
    return tmp;
}

static void MouseDOS_SetMickeyPixelRate(Bit16s px, Bit16s py) {
    if ((px != 0) && (py != 0)) {
        mouse_dos.mickeysPerPixel_x  = static_cast<float>(px) / DOS_X_MICKEY;
        mouse_dos.mickeysPerPixel_y  = static_cast<float>(py) / DOS_Y_MICKEY;
        mouse_dos.pixelPerMickey_x   = DOS_X_MICKEY / static_cast<float>(px);
        mouse_dos.pixelPerMickey_y   = DOS_Y_MICKEY / static_cast<float>(py);    
    }
}

static void MouseDOS_SetSensitivity(Bit16u px, Bit16u py, Bit16u dspeed) {
    px     = std::min(static_cast<Bit16u>(100), px);
    py     = std::min(static_cast<Bit16u>(100), py);
    dspeed = std::min(static_cast<Bit16u>(100), dspeed);
    // save values
    mouse_dos.senv_x_val = px;
    mouse_dos.senv_y_val = py;
    mouse_dos.dspeed_val = dspeed;
    if ((px != 0) && (py != 0)) {
        px--; // Inspired by CuteMouse 
        py--; // Although their cursor update routine is far more complex then ours
        mouse_dos.senv_x = (static_cast<float>(px) * px) / 3600.0f + 1.0f / 3.0f;
        mouse_dos.senv_y = (static_cast<float>(py) * py) / 3600.0f + 1.0f / 3.0f;
     }
}

static void MouseDOS_ResetHardware(void){
    PIC_SetIRQMask(BIOS_MOUSE_IRQ, false);
}

void MouseDOS_BeforeNewVideoMode()
{
    if (CurMode->type!=M_TEXT) RestoreCursorBackground();
    else RestoreCursorBackgroundText();
    mouse_dos.hidden     = 1;
    mouse_dos.oldhidden  = 1;
    mouse_dos.background = false;
}

// FIXME: Does way to much. Many things should be moved to mouse reset one day
void MouseDOS_AfterNewVideoMode(bool setmode) {
    mouse_dos.inhibit_draw = false;
    // Get the correct resolution from the current video mode
    Bit8u mode = mem_readb(BIOS_VIDEO_MODE);
    if (setmode && mode == mouse_dos.mode) LOG(LOG_MOUSE,LOG_NORMAL)("New video mode is the same as the old");
    mouse_dos.gran_x = (Bit16s) 0xffff;
    mouse_dos.gran_y = (Bit16s) 0xffff;
    switch (mode) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x07: {
        mouse_dos.gran_x = (mode < 2) ? 0xfff0 : 0xfff8;
        mouse_dos.gran_y = (Bit16s) 0xfff8;
        Bitu rows = IS_EGAVGA_ARCH ? real_readb(BIOSMEM_SEG,BIOSMEM_NB_ROWS) : 24;
        if ((rows == 0) || (rows > 250)) rows = 25 - 1;
        mouse_dos.max_y = 8 * (rows + 1) - 1;
        break;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0d:
    case 0x0e:
    case 0x13:
        if (mode == 0x0d || mode == 0x13) mouse_dos.gran_x = (Bit16s) 0xfffe;
        mouse_dos.max_y = 199;
        break;
    case 0x0f:
    case 0x10:
        mouse_dos.max_y = 349;
        break;
    case 0x11:
    case 0x12:
        mouse_dos.max_y = 479;
        break;
    default:
        LOG(LOG_MOUSE,LOG_ERROR)("Unhandled videomode %X on reset",mode);
        mouse_dos.inhibit_draw = true;
        return;
    }
    mouse_dos.mode                 = mode;
    mouse_dos.max_x                = 639;
    mouse_dos.min_x                = 0;
    mouse_dos.min_y                = 0;

    mouse_gen.events               = 0;
    mouse_gen.timer_in_progress    = false;
    PIC_RemoveEvents(MouseGEN_EventHandler);

    mouse_dos.hotx                 = 0;
    mouse_dos.hoty                 = 0;
    mouse_dos.screenMask           = DEFAULT_SCREEN_MASK;
    mouse_dos.cursorMask           = DEFAULT_CURSOR_MASK;
    mouse_dos.textAndMask          = DEFAULT_TEXT_AND_MASK;
    mouse_dos.textXorMask          = DEFAULT_TEXT_XOR_MASK;
    mouse_dos.language             = 0;
    mouse_dos.page                 = 0;
    mouse_dos.doubleSpeedThreshold = 64;
    mouse_dos.updateRegion_y[1]    = -1; // offscreen
    mouse_dos.cursorType           = 0;
    mouse_dos.enabled              = true;

    mouse_bios.oldmouseX = static_cast<Bit16s>(mouse_bios.x);
    mouse_bios.oldmouseY = static_cast<Bit16s>(mouse_bios.y);
}

// FIXME: Much too empty, Mouse_NewVideoMode contains stuff that should be in here
static void MouseDOS_Reset()
{
    MouseDOS_BeforeNewVideoMode();
    MouseDOS_AfterNewVideoMode(false);
    MouseDOS_SetMickeyPixelRate(8, 16);

    mouse_dos.mickey_x   = 0;
    mouse_dos.mickey_y   = 0;

    mouse_ps2.buttons    = 0; // XXX should they really be reset here?
    mouse_bios.wheel     = 0;

    mouse_dos.last_wheel_moved_x = 0;
    mouse_dos.last_wheel_moved_y = 0;

    for (Bit16u but = 0; but < DOS_BUTTONS_NUM; but++) {
        mouse_dos.times_pressed[but]   = 0;
        mouse_dos.times_released[but]  = 0;
        mouse_dos.last_pressed_x[but]  = 0;
        mouse_dos.last_pressed_y[but]  = 0;
        mouse_dos.last_released_x[but] = 0;
        mouse_dos.last_released_y[but] = 0;
    }

    // Dont set max coordinates here. it is done by SetResolution!
    mouse_bios.x = static_cast<float>((mouse_dos.max_x + 1) / 2);
    mouse_bios.y = static_cast<float>((mouse_dos.max_y + 1) / 2);
    mouse_dos.sub_mask = 0;
    mouse_dos.in_UIR   = false;
}

static Bitu INT33_Handler(void) {
//    LOG(LOG_MOUSE,LOG_NORMAL)("MOUSE: %04X %X %X %d %d",reg_ax,reg_bx,reg_cx,DOS_GETPOS_X,DOS_GETPOS_Y);
    switch (reg_ax) {
    case 0x00: // MS MOUSE - reset driver and read status
        MouseDOS_ResetHardware();
        [[fallthrough]];
    case 0x21: // MS MOUSE v6.0+ - software reset
        reg_ax = 0xffff;
        reg_bx = DOS_BUTTONS_NUM;
        MouseDOS_Reset();
        break;
    case 0x01: // MS MOUSE v1.0+ - show mouse cursor
        if (mouse_dos.hidden) mouse_dos.hidden--;
        mouse_dos.updateRegion_y[1] = -1; //offscreen
        DrawCursor();
        break;
    case 0x02: // MS MOUSE v1.0+ - hide mouse cursor
        {
            if (CurMode->type != M_TEXT) RestoreCursorBackground();
            else RestoreCursorBackgroundText();
            mouse_dos.hidden++;
        }
        break;
    case 0x03: // MS MOUSE v1.0+ / CuteMouse - return position and button status
        {
            reg_bl = mouse_ps2.buttons & DOS_BUTTONS_MASK;
            reg_bh = MouseBIOS_GetResetWheel8bit();
            reg_cx = DOS_GETPOS_X;
            reg_dx = DOS_GETPOS_Y;
        }
        break;
    case 0x04: // MS MOUSE v1.0+ - position mouse cursor
        // If position isn't different from current position, don't change it.
        // (position is rounded so numbers get lost when the rounded number is set)
        // (arena/simulation Wolf)
        if ((Bit16s) reg_cx >= mouse_dos.max_x) mouse_bios.x = static_cast<float>(mouse_dos.max_x);
        else if (mouse_dos.min_x >= (Bit16s) reg_cx) mouse_bios.x = static_cast<float>(mouse_dos.min_x); 
        else if ((Bit16s) reg_cx != DOS_GETPOS_X) mouse_bios.x = static_cast<float>(reg_cx);

        if ((Bit16s) reg_dx >= mouse_dos.max_y) mouse_bios.y = static_cast<float>(mouse_dos.max_y);
        else if (mouse_dos.min_y >= (Bit16s) reg_dx) mouse_bios.y = static_cast<float>(mouse_dos.min_y); 
        else if ((Bit16s) reg_dx != DOS_GETPOS_Y) mouse_bios.y = static_cast<float>(reg_dx);
        DrawCursor();
        break;
    case 0x05: // MS MOUSE v1.0+ / CuteMouse - return button press data / mouse wheel data
        {
            Bit16u but = reg_bx;
            if (but == 0xffff){
                reg_bx = MouseDOS_GetResetWheel16bit();
                reg_cx = mouse_dos.last_wheel_moved_x;
                reg_dx = mouse_dos.last_wheel_moved_y;
            } else {
                reg_ax = mouse_ps2.buttons & DOS_BUTTONS_MASK;
                if (but >= DOS_BUTTONS_NUM) but = DOS_BUTTONS_NUM - 1;
                reg_cx = mouse_dos.last_pressed_x[but];
                reg_dx = mouse_dos.last_pressed_y[but];
                reg_bx = mouse_dos.times_pressed[but];
                mouse_dos.times_pressed[but] = 0;
            }
        }
        break;
    case 0x06: // MS MOUSE v1.0+ / CuteMouse - return button release data / mouse wheel data
        {
            Bit16u but = reg_bx;
            if (but == 0xffff){
                reg_bx = MouseDOS_GetResetWheel16bit();
                reg_cx = mouse_dos.last_wheel_moved_x;
                reg_dx = mouse_dos.last_wheel_moved_y;
            } else {
                reg_ax = mouse_ps2.buttons & DOS_BUTTONS_MASK;
                if (but >= DOS_BUTTONS_NUM) but = DOS_BUTTONS_NUM - 1;
                reg_cx = mouse_dos.last_released_x[but];
                reg_dx = mouse_dos.last_released_y[but];
                reg_bx = mouse_dos.times_released[but];
                mouse_dos.times_released[but] = 0;
            }
        }
        break;
    case 0x07: // MS MOUSE v1.0+ - define horizontal cursor range
        {   // Lemmings set 1-640 and wants that. iron seeds set 0-640 but doesn't like 640
            // Iron seed works if newvideo mode with mode 13 sets 0-639
            // Larry 6 actually wants newvideo mode with mode 13 to set it to 0-319
            Bit16s max, min;
            if ((Bit16s) reg_cx < (Bit16s) reg_dx) {
            	min = (Bit16s) reg_cx;
            	max = (Bit16s) reg_dx;
            } else {
            	min = (Bit16s) reg_dx;
            	max = (Bit16s) reg_cx;
            }
            mouse_dos.min_x = min;
            mouse_dos.max_x = max;
            // Battlechess wants this
            if(mouse_bios.x > mouse_dos.max_x) mouse_bios.x = mouse_dos.max_x;
            if(mouse_bios.x < mouse_dos.min_x) mouse_bios.x = mouse_dos.min_x;
            // Or alternatively this: 
            // mouse_bios.x = (mouse_dos.max_x - mouse_dos.min_x + 1) / 2;
            LOG(LOG_MOUSE,LOG_NORMAL)("Define Hortizontal range min:%d max:%d", min, max);
        }
        break;
    case 0x08: // MS MOUSE v1.0+ - define vertical cursor range
        {   // not sure what to take instead of the CurMode (see case 0x07 as well)
            // especially the cases where sheight= 400 and we set it with the mouse_reset to 200
            // disabled it at the moment. Seems to break syndicate who want 400 in mode 13
            Bit16s max, min;
            if ((Bit16s) reg_cx < (Bit16s) reg_dx) {
            	min = (Bit16s) reg_cx;
            	max = (Bit16s) reg_dx;
            } else {
            	min = (Bit16s) reg_dx;
            	max = (Bit16s) reg_cx;
            }
            mouse_dos.min_y = min;
            mouse_dos.max_y = max;
            /* Battlechess wants this */
            if(mouse_bios.y > mouse_dos.max_y) mouse_bios.y = mouse_dos.max_y;
            if(mouse_bios.y < mouse_dos.min_y) mouse_bios.y = mouse_dos.min_y;
            /* Or alternatively this: 
            mouse_bios.y = (mouse_dos.max_y - mouse_dos.min_y + 1)/2;*/
            LOG(LOG_MOUSE,LOG_NORMAL)("Define Vertical range min:%d max:%d", min, max);
        }
        break;
    case 0x09: // MS MOUSE v3.0+ - define GFX cursor
        {
            PhysPt src = SegPhys(es)+reg_dx;
            MEM_BlockRead(src                   , userdefScreenMask, DOS_Y_CURSOR * 2);
            MEM_BlockRead(src + DOS_Y_CURSOR * 2, userdefCursorMask, DOS_Y_CURSOR * 2);
            mouse_dos.screenMask = userdefScreenMask;
            mouse_dos.cursorMask = userdefCursorMask;
            mouse_dos.hotx       = reg_bx;
            mouse_dos.hoty       = reg_cx;
            mouse_dos.cursorType = 2;
            DrawCursor();
        }
        break;
    case 0x0a: // MS MOUSE v3.0+ - define text cursor
        mouse_dos.cursorType  = (reg_bx ? 1 : 0);
        mouse_dos.textAndMask = reg_cx;
        mouse_dos.textXorMask = reg_dx;
        if (reg_bx) {
            INT10_SetCursorShape(reg_cl, reg_dl);
            LOG(LOG_MOUSE,LOG_NORMAL)("Hardware Text cursor selected");
        }
        DrawCursor();
        break;
    case 0x27: // MS MOUSE v7.01+ - get screen/cursor masks and mickey counts
        reg_ax = mouse_dos.textAndMask;
        reg_bx = mouse_dos.textXorMask;
        [[fallthrough]];
    case 0x0b: // MS MOUSE v1.0+ - read motion data
        reg_cx=static_cast<Bit16s>(mouse_dos.mickey_x);
        reg_dx=static_cast<Bit16s>(mouse_dos.mickey_y);
        mouse_dos.mickey_x=0;
        mouse_dos.mickey_y=0;
        break;
    case 0x0c: // MS MOUSE v1.0+ - define interrupt subroutine parameters
        mouse_dos.sub_mask = reg_cx & DOS_EVENTS_MASK;
        mouse_dos.sub_seg  = SegValue(es);
        mouse_dos.sub_ofs  = reg_dx;
        break;
    case 0x0d: // MS MOUSE v1.0+ - light pen emulation on
    case 0x0e: // MS MOUSE v1.0+ - light pen emulation off
        LOG(LOG_MOUSE,LOG_ERROR)("Mouse light pen emulation not implemented");
        break;
    case 0x0f: // MS MOUSE v1.0+ - define mickey/pixel rate
        MouseDOS_SetMickeyPixelRate(reg_cx, reg_dx);
        break;
    case 0x10: // MS MOUSE v1.0+ - define screen region for updating
        mouse_dos.updateRegion_x[0]=(Bit16s) reg_cx;
        mouse_dos.updateRegion_y[0]=(Bit16s) reg_dx;
        mouse_dos.updateRegion_x[1]=(Bit16s) reg_si;
        mouse_dos.updateRegion_y[1]=(Bit16s) reg_di;
        DrawCursor();
        break;
    case 0x11: // CuteMouse - get mouse capabilities
        reg_ax = 0x574D; // Identifier for detection purposes
        reg_bx = 0;      // Reserved capabilities flags
        reg_cx = 1;      // Wheel is supported
        // Previous implementation provided Genius Mouse 9.06 function to get
        // number of buttons (https://sourceforge.net/p/dosbox/patches/32/), it was
        // returning 0xffff in reg_ax and number of buttons in reg_bx; I suppose
        // the CuteMouse extensions are more useful
        break;
    case 0x12: // MS MOUSE - set large graphics cursor block
        LOG(LOG_MOUSE,LOG_ERROR)("Large graphics cursor block not implemented");
        break;
    case 0x13: // MS MOUSE v5.0+ - set double-speed threshold
        mouse_dos.doubleSpeedThreshold = (reg_bx ? reg_bx : 64);
         break;
    case 0x14: // MS MOUSE v3.0+ - exchange event-handler 
        {    
            Bit16u oldSeg  = mouse_dos.sub_seg;
            Bit16u oldOfs  = mouse_dos.sub_ofs;
            Bit16u oldMask = mouse_dos.sub_mask;
            // Set new values
            mouse_dos.sub_mask = reg_cx;
            mouse_dos.sub_seg  = SegValue(es);
            mouse_dos.sub_ofs  = reg_dx;
            // Return old values
            reg_cx = oldMask;
            reg_dx = oldOfs;
            SegSet16(es, oldSeg);
        }
        break;        
    case 0x15: // MS MOUSE v6.0+ - get driver storage space requirements
        reg_bx = sizeof(mouse_dos);
        break;
    case 0x16: // MS MOUSE v6.0+ - save driver state
        {
            LOG(LOG_MOUSE,LOG_WARN)("Saving driver state...");
            PhysPt dest = SegPhys(es) + reg_dx;
            MEM_BlockWrite(dest, &mouse_dos, sizeof(mouse_dos));
        }
        break;
    case 0x17: // MS MOUSE v6.0+ - load driver state
        {
            LOG(LOG_MOUSE,LOG_WARN)("Loading driver state...");
            PhysPt src = SegPhys(es) + reg_dx;
            MEM_BlockRead(src, &mouse_dos, sizeof(mouse_dos));
        }
        break;
    case 0x18: // MS MOUSE v6.0+ - set alternate mouse user handler
    case 0x19: // MS MOUSE v6.0+ - set alternate mouse user handler
        LOG(LOG_MOUSE,LOG_WARN)("Alternate mouse user handler not implemented");
        break;
    case 0x1a: // MS MOUSE v6.0+ - set mouse sensitivity
        // FIXME : double mouse speed value
        MouseDOS_SetSensitivity(reg_bx, reg_cx, reg_dx);

        LOG(LOG_MOUSE,LOG_WARN)("Set sensitivity used with %d %d (%d)",reg_bx,reg_cx,reg_dx);
        break;
    case 0x1b: //  MS MOUSE v6.0+ - get mouse sensitivity
        reg_bx = mouse_dos.senv_x_val;
        reg_cx = mouse_dos.senv_y_val;
        reg_dx = mouse_dos.dspeed_val;

        LOG(LOG_MOUSE,LOG_WARN)("Get sensitivity %d %d",reg_bx,reg_cx);
        break;
    case 0x1c: // MS MOUSE v6.0+ - set interrupt rate
        // Can't really set a rate this is host determined
        break;
    case 0x1d: // MS MOUSE v6.0+ - set display page number
        mouse_dos.page = reg_bl;
        break;
    case 0x1e: // MS MOUSE v6.0+ - get display page number
        reg_bx = mouse_dos.page;
        break;
    case 0x1f: // MS MOUSE v6.0+ - disable mouse driver
        // ES:BX old mouse driver Zero at the moment TODO
        reg_bx = 0;
        SegSet16(es, 0);       
        mouse_dos.enabled   = false; /* Just for reporting not doing a thing with it */
        mouse_dos.oldhidden = mouse_dos.hidden;
        mouse_dos.hidden    = 1;
        break;
    case 0x20: // MS MOUSE v6.0+ - enable mouse driver
        mouse_dos.enabled   = true;
        mouse_dos.hidden    = mouse_dos.oldhidden;
        break;
    case 0x22: // MS MOUSE v6.0+ - set language for messages
        // 00h = English, 01h = French, 02h = Dutch, 03h = German, 04h = Swedish
        // 05h = Finnish, 06h = Spanish, 07h = Portugese, 08h = Italian
        mouse_dos.language = reg_bx;
        break;
    case 0x23: // MS MOUSE v6.0+ - get language for messages
        reg_bx = mouse_dos.language;
        break;
    case 0x24: // MS MOUSE v6.26+ - get Software version, mouse type, and IRQ number
        reg_bx = 0x805; // version 8.05 woohoo 
        reg_ch = 0x04;  // PS/2 type
        reg_cl = 0;     // PS/2 mouse; for any other type it would be IRQ number
        break;
    case 0x25: // MS MOUSE v6.26+ - get general driver information
        // TODO: According to PC sourcebook reference
        //       Returns:
        //       AH = status
        //         bit 7 driver type: 1=sys 0=com
        //         bit 6: 0=non-integrated 1=integrated mouse driver
        //         bits 4-5: cursor type  00=software text cursor 01=hardware text cursor 1X=graphics cursor
        //         bits 0-3: Function 28 mouse interrupt rate
        //       AL = Number of MDDS (?)
        //       BX = fCursor lock
        //       CX = FinMouse code
        //       DX = fMouse busy
        LOG(LOG_MOUSE,LOG_ERROR)("General driver information not implemented");
        break;
    case 0x26: // MS MOUSE v6.26+ - get maximum virtual coordinates
        reg_bx = (mouse_dos.enabled ? 0x0000 : 0xffff);
        reg_cx = (Bit16u) mouse_dos.max_x;
        reg_dx = (Bit16u) mouse_dos.max_y;
        break;
    case 0x28: // MS MOUSE v7.0+ - set video mode
        // TODO: According to PC sourcebook
        //       Entry:
        //       CX = Requested video mode
        //       DX = Font size, 0 for default
        //       Returns:
        //       DX = 0 on success, nonzero (requested video mode) if not
        LOG(LOG_MOUSE,LOG_ERROR)("Set video mode not implemented");
        break;
    case 0x29: // MS MOUSE v7.0+ - enumerate video modes
        // TODO: According to PC sourcebook
        //       Entry:
        //       CX = 0 for first, != 0 for next
        //       Exit:
        //       BX:DX = named string far ptr
        //       CX = video mode number
        LOG(LOG_MOUSE,LOG_ERROR)("Enumerate video modes not implemented");
        break;
    case 0x2a: // MS MOUSE v7.01+ - get cursor hot spot
        reg_al = (Bit8u) -mouse_dos.hidden;    // Microsoft uses a negative byte counter for cursor visibility
        reg_bx = (Bit16u) mouse_dos.hotx;
        reg_cx = (Bit16u) mouse_dos.hoty;
        reg_dx = 0x04;    // PS/2 mouse type
        break;
    case 0x2b: // MS MOUSE v7.0+ - load acceleration profiles
        LOG(LOG_MOUSE,LOG_ERROR)("Load acceleration profiles not implemented");
        break;
    case 0x2c: // MS MOUSE v7.0+ - get acceleration profiles
        LOG(LOG_MOUSE,LOG_ERROR)("Get acceleration profiles not implemented");
        break;
    case 0x2d: // MS MOUSE v7.0+ - select acceleration profile
        LOG(LOG_MOUSE,LOG_ERROR)("Select acceleration profile not implemented");
        break;
    case 0x2e: // MS MOUSE v8.10+ - set acceleration profile names
        LOG(LOG_MOUSE,LOG_ERROR)("Set acceleration profile names not implemented");
        break;
    case 0x2f: // MS MOUSE v7.02+ - mouse hardware reset
        LOG(LOG_MOUSE,LOG_ERROR)("INT 33 AX=2F mouse hardware reset not implemented");
        break;
    case 0x30: // MS MOUSE v7.04+ - get/set BallPoint information
        LOG(LOG_MOUSE,LOG_ERROR)("Get/set BallPoint information not implemented");
        break;
    case 0x31: // MS MOUSE v7.05+ - get current minimum/maximum virtual coordinates
        reg_ax = (Bit16u) mouse_dos.min_x;
        reg_bx = (Bit16u) mouse_dos.min_y;
        reg_cx = (Bit16u) mouse_dos.max_x;
        reg_dx = (Bit16u) mouse_dos.max_y;
        break;
    case 0x32: // MS MOUSE v7.05+ - get active advanced functions
        LOG(LOG_MOUSE,LOG_ERROR)("Get active advanced functions not implemented");
        break;
    case 0x33: // MS MOUSE v7.05+ - get switch settings and accelleration profile data
        LOG(LOG_MOUSE,LOG_ERROR)("Get switch settings and acceleration profile data not implemented");
        break;
    case 0x34: // MS MOUSE v8.0+ - get initialization file
        LOG(LOG_MOUSE,LOG_ERROR)("Get initialization file not implemented");
        break;
    case 0x35: // MS MOUSE v8.10+ - LCD screen large pointer support
        LOG(LOG_MOUSE,LOG_ERROR)("LCD screen large pointer support not implemented");
        break;
    case 0x4d: // MS MOUSE - return pointer to copyright string
        LOG(LOG_MOUSE,LOG_ERROR)("Return pointer to copyright string not implemented");
        break;
    case 0x6d: // MS MOUSE - get version string
        LOG(LOG_MOUSE,LOG_ERROR)("Get version string not implemented");
        break;
    case 0x53C1: // Logitech CyberMan
        LOG(LOG_MOUSE,LOG_NORMAL)("Mouse function 53C1 for Logitech CyberMan called. Ignored by regular mouse driver.");
        break;
    default:
        LOG(LOG_MOUSE,LOG_ERROR)("Mouse Function %04X not implemented!",reg_ax);
        break;
    }
    return CBRET_NONE;
}

static Bitu MOUSE_BD_Handler(void) {
    // the stack contains offsets to register values
    Bit16u raxpt=real_readw(SegValue(ss),reg_sp+0x0a);
    Bit16u rbxpt=real_readw(SegValue(ss),reg_sp+0x08);
    Bit16u rcxpt=real_readw(SegValue(ss),reg_sp+0x06);
    Bit16u rdxpt=real_readw(SegValue(ss),reg_sp+0x04);

    // read out the actual values, registers ARE overwritten
    Bit16u rax=real_readw(SegValue(ds),raxpt);
    reg_ax=rax;
    reg_bx=real_readw(SegValue(ds),rbxpt);
    reg_cx=real_readw(SegValue(ds),rcxpt);
    reg_dx=real_readw(SegValue(ds),rdxpt);
//    LOG_MSG("MOUSE BD: %04X %X %X %X %d %d",reg_ax,reg_bx,reg_cx,reg_dx,DOS_GETPOS_X,DOS_GETPOS_Y);
    
    // some functions are treated in a special way (additional registers)
    switch (rax) {
        case 0x09:    /* Define GFX Cursor */
        case 0x16:    /* Save driver state */
        case 0x17:    /* load driver state */
            SegSet16(es,SegValue(ds));
            break;
        case 0x0c:    /* Define interrupt subroutine parameters */
        case 0x14:    /* Exchange event-handler */ 
            if (reg_bx!=0) SegSet16(es,reg_bx);
            else SegSet16(es,SegValue(ds));
            break;
        case 0x10:    /* Define screen region for updating */
            reg_cx=real_readw(SegValue(ds),rdxpt);
            reg_dx=real_readw(SegValue(ds),rdxpt+2);
            reg_si=real_readw(SegValue(ds),rdxpt+4);
            reg_di=real_readw(SegValue(ds),rdxpt+6);
            break;
        default:
            break;
    }

    INT33_Handler();

    // save back the registers, too
    real_writew(SegValue(ds),raxpt,reg_ax);
    real_writew(SegValue(ds),rbxpt,reg_bx);
    real_writew(SegValue(ds),rcxpt,reg_cx);
    real_writew(SegValue(ds),rdxpt,reg_dx);
    switch (rax) {
        case 0x1f:    /* Disable Mousedriver */
            real_writew(SegValue(ds),rbxpt,SegValue(es));
            break;
        case 0x14: /* Exchange event-handler */ 
            real_writew(SegValue(ds),rcxpt,SegValue(es));
            break;
        default:
            break;
    }

    reg_ax=rax;
    return CBRET_NONE;
}

static Bitu INT74_Handler(void) {
    if (mouse_gen.events > 0 && !mouse_dos.in_UIR) {
        mouse_gen.events--;
        if (mouse_dos.sub_mask & mouse_gen.event_queue[mouse_gen.events].type) {
            // DOS interrupt handler is installed - call it
            reg_ax = mouse_gen.event_queue[mouse_gen.events].type;
            reg_bl = mouse_gen.event_queue[mouse_gen.events].buttons;
            reg_bh = MouseBIOS_GetResetWheel8bit();
            reg_cx = DOS_GETPOS_X;
            reg_dx = DOS_GETPOS_Y;
            reg_si = static_cast<Bit16s>(mouse_dos.mickey_x);
            reg_di = static_cast<Bit16s>(mouse_dos.mickey_y);
            CPU_Push16(RealSeg(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback)));
            CPU_Push16(RealOff(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback)) + 7);
            CPU_Push16(RealSeg(mouse_nosave.uir_callback));
            CPU_Push16(RealOff(mouse_nosave.uir_callback));
            CPU_Push16(mouse_dos.sub_seg);
            CPU_Push16(mouse_dos.sub_ofs);
            mouse_dos.in_UIR = true;
            //LOG(LOG_MOUSE,LOG_ERROR)("INT 74 %X",mouse_gen.event_queue[mouse_gen.events].type );
        } else if (mouse_bios.useps2callback) {
            CPU_Push16(RealSeg(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback)));
            CPU_Push16(RealOff(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback)));
            MouseBIOS_DoPS2Callback(mouse_gen.event_queue[mouse_gen.events].buttons,
                                    static_cast<Bit16s>(mouse_bios.x),
                                    static_cast<Bit16s>(mouse_bios.y));
        } else {
            SegSet16(cs, RealSeg(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback)));
            reg_ip = RealOff(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback));
            //LOG(LOG_MOUSE,LOG_ERROR)("INT 74 not interested"); 
        }
    } else {
        SegSet16(cs, RealSeg(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback)));
        reg_ip = RealOff(CALLBACK_RealPointer(mouse_nosave.int74_ret_callback));
        //LOG(LOG_MOUSE,LOG_ERROR)("INT 74 no events");
    }
    return CBRET_NONE;
}

Bitu INT74_Ret_Handler(void) {
    if (mouse_gen.events) {
        if (!mouse_gen.timer_in_progress) {
            mouse_gen.timer_in_progress = true;
            PIC_AddEvent(MouseGEN_EventHandler, mouse_ps2.delay);
        }
    }
    return CBRET_NONE;
}

Bitu UIR_Handler(void) {
    mouse_dos.in_UIR = false;
    return CBRET_NONE;
}

// ***************************************************************************
// Generic functionality, related to multiple protocols
// ***************************************************************************

static void MouseGEN_EventHandler(uint32_t /*val*/)
{
    mouse_gen.timer_in_progress = false;
    if (mouse_gen.events) {
        mouse_gen.timer_in_progress = true;
        PIC_AddEvent(MouseGEN_EventHandler, mouse_ps2.delay);
        PIC_ActivateIRQ(BIOS_MOUSE_IRQ);
    }
}

static void MouseGEN_AddEvent(Bit16u type) {
    if (mouse_gen.events < GEN_QUEUE_SIZE) {
        if (mouse_gen.events > 0) {
            // Skip duplicate events
            if (type == DOS_EV::MOUSE_MOVED || type == DOS_EV::WHEEL_MOVED) return;
            // Always put the newest element in the front as that the events are 
            // handled backwards (prevents doubleclicks while moving)
            for(Bitu i = mouse_gen.events ; i ; i--)
                mouse_gen.event_queue[i] = mouse_gen.event_queue[i - 1];
        }
        mouse_gen.event_queue[0].type    = type;
        mouse_gen.event_queue[0].buttons = mouse_ps2.buttons;
        mouse_gen.events++;
    }
    if (!mouse_gen.timer_in_progress) {
        mouse_gen.timer_in_progress = true;
        PIC_AddEvent(MouseGEN_EventHandler, mouse_ps2.delay);
        PIC_ActivateIRQ(BIOS_MOUSE_IRQ);
    }
}

static void MouseGEN_CursorMoved(Bit32s x_rel, Bit32s y_rel, bool is_captured) {

    float x_rel_sens = x_rel * config.sensitivity_x;
    float y_rel_sens = y_rel * config.sensitivity_y;

    float dx = x_rel_sens * mouse_dos.pixelPerMickey_x;
    float dy = y_rel_sens * mouse_dos.pixelPerMickey_y;

    if((fabs(x_rel_sens) > 1.0) || (mouse_dos.senv_x < 1.0)) dx *= mouse_dos.senv_x;
    if((fabs(y_rel_sens) > 1.0) || (mouse_dos.senv_y < 1.0)) dy *= mouse_dos.senv_y;
    if (mouse_bios.useps2callback) dy *= 2;    

    mouse_dos.mickey_x += (dx * mouse_dos.mickeysPerPixel_x);
    mouse_dos.mickey_y += (dy * mouse_dos.mickeysPerPixel_y);
    if (mouse_dos.mickey_x >= 32768.0) mouse_dos.mickey_x -= 65536.0;
    else if (mouse_dos.mickey_x <= -32769.0) mouse_dos.mickey_x += 65536.0;
    if (mouse_dos.mickey_y >= 32768.0) mouse_dos.mickey_y -= 65536.0;
    else if (mouse_dos.mickey_y <= -32769.0) mouse_dos.mickey_y += 65536.0;
    if (is_captured) {
        mouse_bios.x += dx;
        mouse_bios.y += dy;
    } else {
        float x = (x_rel - video.clip_x) / (video.res_x - 1) * config.sensitivity_x;
        float y = (y_rel - video.clip_y) / (video.res_y - 1) * config.sensitivity_y;

        if (CurMode->type == M_TEXT) {
            mouse_bios.x = x * real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS) * 8;
            mouse_bios.y = y * (IS_EGAVGA_ARCH ? (real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1) : 25) * 8;
        } else if ((mouse_dos.max_x < 2048) || (mouse_dos.max_y < 2048) || (mouse_dos.max_x != mouse_dos.max_y)) {
            if ((mouse_dos.max_x > 0) && (mouse_dos.max_y > 0)) {
                mouse_bios.x = x * mouse_dos.max_x;
                mouse_bios.y = y * mouse_dos.max_y;
            } else {
                mouse_bios.x += x_rel_sens;
                mouse_bios.y += y_rel_sens;
            }
        } else { // Games faking relative movement through absolute coordinates. Quite surprising that this actually works..
            mouse_bios.x += x_rel_sens;
            mouse_bios.y += y_rel_sens;
        }
    }

    // ignore constraints if using PS2 mouse callback in the bios

    if (!mouse_bios.useps2callback) {        
        if (mouse_bios.x > mouse_dos.max_x) mouse_bios.x = mouse_dos.max_x;
        if (mouse_bios.x < mouse_dos.min_x) mouse_bios.x = mouse_dos.min_x;
        if (mouse_bios.y > mouse_dos.max_y) mouse_bios.y = mouse_dos.max_y;
        if (mouse_bios.y < mouse_dos.min_y) mouse_bios.y = mouse_dos.min_y;
    } else {
        if (mouse_bios.x >= 32768.0) mouse_bios.x -= 65536.0;
        else if (mouse_bios.x <= -32769.0) mouse_bios.x += 65536.0;
        if (mouse_bios.y >= 32768.0) mouse_bios.y -= 65536.0;
        else if (mouse_bios.y <= -32769.0) mouse_bios.y += 65536.0;
    }

    MouseGEN_AddEvent(DOS_EV::MOUSE_MOVED);
    DrawCursor();
}

// ***************************************************************************
// External notifications
// ***************************************************************************

void Mouse_SetSensitivity(Bit32s sensitivity_x, Bit32s sensitivity_y) {
    static constexpr float MIN = 0.01f;
    static constexpr float MAX = 100.0f;

    config.sensitivity_x = std::clamp(sensitivity_x/100.0f, -MAX, MAX);
    if (!std::signbit(config.sensitivity_x))
        config.sensitivity_x = std::max(config.sensitivity_x, MIN);
    else
        config.sensitivity_x = std::min(config.sensitivity_x, -MIN);

    config.sensitivity_y = std::clamp(sensitivity_y/100.0f, -MAX, MAX);
    if (!std::signbit(config.sensitivity_y))
        config.sensitivity_y = std::max(config.sensitivity_y, MIN);
    else
        config.sensitivity_y = std::min(config.sensitivity_y, -MIN);
}

void Mouse_NewScreenParams(Bit16u clip_x, Bit16u clip_y,
                           Bit16u res_x,  Bit16u res_y,
                           bool fullscreen,
                           Bit32s x_abs, Bit32s y_abs) {

    video.clip_x     = clip_x;
    video.clip_y     = clip_y;
    video.res_x      = res_x;
    video.res_y      = res_y;
    video.fullscreen = fullscreen;

    // Handle VMware driver - adjust clipping, prevent cursor jump with the next mouse move on the host side

    mouse_vmw.offset_x = std::clamp(static_cast<Bit32s>(mouse_vmw.offset_x), -video.clip_x, static_cast<Bit32s>(video.clip_x));
    mouse_vmw.offset_y = std::clamp(static_cast<Bit32s>(mouse_vmw.offset_y), -video.clip_y, static_cast<Bit32s>(video.clip_y));

    MouseVMW_CursorMoved(x_abs, y_abs);
    if (mouse_vmware)
        MouseGEN_AddEvent(DOS_EV::MOUSE_MOVED);
}

void Mouse_CursorMoved(Bit32s x_rel, Bit32s y_rel, Bit32s x_abs, Bit32s y_abs, bool is_captured) {
    MouseVMW_CursorMoved(x_abs, y_abs);
    MouseGEN_CursorMoved(x_rel, y_rel, is_captured);
    MouseSERIAL_CursorMoved(x_rel, y_rel);
}

void Mouse_ButtonPressed(Bit8u button) {
    if (button >= 5) return; // absolutely no support for buttons above 5

    bool ps2_updated        = false;
    bool ps2_squish         = MousePS2_IsButtonSquishMode();
    auto old_serial_buttons = mouse_serial.buttons;
    switch (button) {
    case 0:
        mouse_serial.buttons |= 1;
        if (!(mouse_vmw.buttons & VMW_BUTTON::LEFT)) {
            mouse_vmw.buttons |= VMW_BUTTON::LEFT;
            mouse_vmw.updated  = true;
        }
        if (!(mouse_ps2.buttons & 1)) {
            mouse_ps2.buttons |= 1;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::PRESSED_LEFT);
        }
        break;
    case 1:
        mouse_serial.buttons |= 2;
        if (!(mouse_vmw.buttons & VMW_BUTTON::RIGHT)) {
            mouse_vmw.buttons |= VMW_BUTTON::RIGHT;
            mouse_vmw.updated  = true;
        }
        if (!(mouse_ps2.buttons & 2)) {
            mouse_ps2.buttons |= 2;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::PRESSED_RIGHT);
        }
        break;
    case 2:
        mouse_gen.buttons_extra |= 1;
        if (!ps2_squish && !(mouse_ps2.buttons & 4)) {
            mouse_ps2.buttons |= 4;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::PRESSED_MIDDLE);
        }
        break;
    case 3:
        mouse_gen.buttons_extra |= 2;
        if (!ps2_squish && !(mouse_ps2.buttons & 8)) {
            mouse_ps2.buttons |= 8;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::PRESSED_X1);
        }
        break;
    case 4:
        mouse_gen.buttons_extra |= 4;
        if (!ps2_squish && !(mouse_ps2.buttons & 16)) {
            mouse_ps2.buttons |= 16;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::PRESSED_X2);
        }
        break;
    }

    // If the interface does not support buttons 3/4/5, squish them all into button 3
    if ((button >= 2) && mouse_gen.buttons_extra) {
        mouse_serial.buttons |= 4;
        if (!(mouse_vmw.buttons & VMW_BUTTON::MIDDLE)) {
            mouse_vmw.buttons |= VMW_BUTTON::MIDDLE;
            mouse_vmw.updated  = true;
        }
        if (ps2_squish && !(mouse_ps2.buttons & 4)) {
            mouse_ps2.buttons |= 4;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::PRESSED_MIDDLE);
        }
    }

    if (ps2_updated && (button < DOS_BUTTONS_NUM)) {
        mouse_dos.times_pressed[button]++;
        mouse_dos.last_pressed_x[button] = DOS_GETPOS_X;
        mouse_dos.last_pressed_y[button] = DOS_GETPOS_Y;      
    }

    if (old_serial_buttons != mouse_serial.buttons)
            MouseSERIAL_Button(button);
}

void Mouse_ButtonReleased(Bit8u button) {
    if (button >= 5) return; // absolutely no support for buttons above 5

    bool ps2_updated = false;
    bool ps2_squish  = MousePS2_IsButtonSquishMode();
    auto old_serial_buttons = mouse_serial.buttons;
    switch (button) {
    case 0:
        mouse_serial.buttons &= ~1;
        if (mouse_vmw.buttons & VMW_BUTTON::LEFT) {
            mouse_vmw.buttons &= ~VMW_BUTTON::LEFT;
            mouse_vmw.updated  = true;
        }
        if (mouse_ps2.buttons & 1) {
            mouse_ps2.buttons &= ~1;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::RELEASED_LEFT);
        }
        break;
    case 1:
        mouse_serial.buttons &= ~2;
        if (mouse_vmw.buttons & VMW_BUTTON::RIGHT) {
            mouse_vmw.buttons &= ~VMW_BUTTON::RIGHT;
            mouse_vmw.updated  = true;
        }
        if (mouse_ps2.buttons & 2) {
            mouse_ps2.buttons &= ~2;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::RELEASED_RIGHT);
        }
        break;
    case 2:
        mouse_gen.buttons_extra &= ~1;
        if (!ps2_squish && (mouse_ps2.buttons & 4)) {
            mouse_ps2.buttons &= ~4;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::RELEASED_MIDDLE);
        }
        break;
    case 3:
        mouse_gen.buttons_extra &= ~2;
        if (!ps2_squish && (mouse_ps2.buttons & 8)) {
            mouse_ps2.buttons &= ~8;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::RELEASED_X1);
        }
        break;
    case 4:
        mouse_gen.buttons_extra &= ~4;
        if (!ps2_squish && (mouse_ps2.buttons & 16)) {
            mouse_ps2.buttons &= ~16;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::RELEASED_X2);
        }
        break;
    }

    // If the interface does not support buttons 3/4/5, squish them all into button 3
    if ((button >= 2) && !mouse_gen.buttons_extra) {
        mouse_serial.buttons &= ~4;
        if (mouse_vmw.buttons & VMW_BUTTON::MIDDLE) {
            mouse_vmw.buttons &= ~VMW_BUTTON::MIDDLE;
            mouse_vmw.updated  = true;
        }
        if (ps2_squish && (mouse_ps2.buttons & 4)) {
            mouse_ps2.buttons &= ~4;
            ps2_updated        = true;
            MouseGEN_AddEvent(DOS_EV::RELEASED_MIDDLE);
        }
    }

    if (ps2_updated && (button < DOS_BUTTONS_NUM)) {
        mouse_dos.times_released[button]++;    
        mouse_dos.last_released_x[button] = DOS_GETPOS_X;
        mouse_dos.last_released_y[button] = DOS_GETPOS_Y;
    }

    if (old_serial_buttons != mouse_serial.buttons)
            MouseSERIAL_Button(button);
}

void Mouse_WheelMoved(Bit32s scroll) {
    // API limits are -0x80,0x7F - but let's keep it symmetric
    MouseSERIAL_WheelMoved(std::clamp(scroll, -0x7F, 0x7F));

    if (mouse_vmware) {
        // API limits are -128,127 - but let's keep it symmetric
        mouse_vmw.wheel   = std::clamp(scroll + mouse_vmw.wheel, -127, 127);
        mouse_vmw.updated = true;
    }

    // API limits are -0x8000,0x7FFF - but let's keep it symmetric
    mouse_bios.wheel = std::clamp(scroll + mouse_bios.wheel, -0x7FFF, 0x7FFF);
    MouseGEN_AddEvent(DOS_EV::WHEEL_MOVED);
    mouse_dos.last_wheel_moved_x = DOS_GETPOS_X;
    mouse_dos.last_wheel_moved_y = DOS_GETPOS_Y;
}

void MOUSE_Init(Section* /*sec*/) {
    video.Init();
    mouse_ps2.Init();
    mouse_vmw.Init();
    mouse_bios.Init();
    mouse_dos.Init();
    mouse_nosave.Init();
    mouse_gen.Init();

    // Callback for mouse interrupt 0x33
    mouse_nosave.call_int33 = CALLBACK_Allocate();
    // RealPt i33loc=RealMake(CB_SEG+1,(mouse_nosave.call_int33*CB_SIZE)-0x10);
    RealPt i33loc = RealMake(DOS_GetMemory(0x1) - 1, 0x10);
    CALLBACK_Setup(mouse_nosave.call_int33, &INT33_Handler, CB_MOUSE, Real2Phys(i33loc), "Mouse");
    // Wasteland needs low(seg(int33))!=0 and low(ofs(int33))!=0
    real_writed(0, 0x33 << 2, i33loc);

    mouse_nosave.call_mouse_bd=CALLBACK_Allocate();
    CALLBACK_Setup(mouse_nosave.call_mouse_bd, &MOUSE_BD_Handler, CB_RETF8,
                PhysMake(RealSeg(i33loc), RealOff(i33loc) + 2), "MouseBD");
    // pseudocode for CB_MOUSE (including the special backdoor entry point):
    //    jump near i33hd
    //    callback MOUSE_BD_Handler
    //    retf 8
    //  label i33hd:
    //    callback INT33_Handler
    //    iret


    // Callback for ps2 irq
    mouse_nosave.call_int74 = CALLBACK_Allocate();
    CALLBACK_Setup(mouse_nosave.call_int74, &INT74_Handler, CB_IRQ12, "int 74");
    // pseudocode for CB_IRQ12:
    //    sti
    //    push ds
    //    push es
    //    pushad
    //    callback INT74_Handler
    //        ps2 or user callback if requested
    //        otherwise jumps to CB_IRQ12_RET
    //    push ax
    //    mov al, 0x20
    //    out 0xa0, al
    //    out 0x20, al
    //    pop    ax
    //    cld
    //    retf

    mouse_nosave.int74_ret_callback = CALLBACK_Allocate();
    CALLBACK_Setup(mouse_nosave.int74_ret_callback, &INT74_Ret_Handler, CB_IRQ12_RET, "int 74 ret");
    // pseudocode for CB_IRQ12_RET:
    //    cli
    //    mov al, 0x20
    //    out 0xa0, al
    //    out 0x20, al
    //    callback INT74_Ret_Handler
    //    popad
    //    pop es
    //    pop ds
    //    iret

    Bit8u hwvec = (BIOS_MOUSE_IRQ > 7) ? (0x70 + BIOS_MOUSE_IRQ - 8) : (0x8 + BIOS_MOUSE_IRQ);
    RealSetVec(hwvec, CALLBACK_RealPointer(mouse_nosave.call_int74));

    // Callback for ps2 user callback handling
    mouse_bios.call_ps2 = CALLBACK_Allocate();
    CALLBACK_Setup(mouse_bios.call_ps2, &MouseBIOS_PS2_DummyHandler, CB_RETF, "ps2 bios callback");
    mouse_bios.ps2_callback = CALLBACK_RealPointer(mouse_bios.call_ps2);

    // Callback for mouse user routine return
    mouse_nosave.call_uir = CALLBACK_Allocate();
    CALLBACK_Setup(mouse_nosave.call_uir, &UIR_Handler, CB_RETF_CLI, "mouse uir ret");
    mouse_nosave.uir_callback = CALLBACK_RealPointer(mouse_nosave.call_uir);

    // VMware I/O port
    IO_RegisterReadHandler(VMW_PORT, MouseVMW_PortRead, io_width_t::word, 1);

    MousePS2_Reset();
    MouseDOS_ResetHardware();
    MouseDOS_Reset();
    MouseDOS_SetSensitivity(50, 50, 50);
}
