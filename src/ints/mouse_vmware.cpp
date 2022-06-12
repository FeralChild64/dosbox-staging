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

#include "mouse.h"

#include <algorithm>

#include "checks.h"
#include "regs.h"
#include "inout.h"
#include "video.h"

CHECK_NARROWING();

// VMware mouse interface passes both absolute mouse position and button
// state to the guest side driver, but still relies on PS/2 interface,
// which has to be used to listen for events

// Reference:
// - https://wiki.osdev.org/VMware_tools
// - https://wiki.osdev.org/VirtualBox_Guest_Additions (planned support)
// Drivers:
// - https://github.com/NattyNarwhal/vmwmouse
// - https://git.javispedro.com/cgit/vbmouse.git (planned support)

enum VMwareCmd:uint16_t {
    GETVERSION         = 10,
    ABSPOINTER_DATA    = 39,
    ABSPOINTER_STATUS  = 40,
    ABSPOINTER_COMMAND = 41,
};

enum VMwareAbsPointer:uint32_t {
    ENABLE             = 0x45414552,
    RELATIVE           = 0xF5,
    ABSOLUTE           = 0x53424152,
};

enum VMwareButton:uint8_t {
    LEFT               = 0x20,
    RIGHT              = 0x10,
    MIDDLE             = 0x08,
};

static constexpr io_port_t VMWARE_PORT   = 0x5658u;     // communication port
// static constexpr io_port_t VMWARE_PORTHB = 0x5659u;  // communication port, high bandwidth
static constexpr uint32_t  VMWARE_MAGIC  = 0x564D5868u; // magic number for all VMware calls

static bool      updated        = false;  // true = mouse state update waits top be piced up
static uint8_t   buttons_vmware = 0;      // state of mouse buttons, in VMware format
static uint16_t  scaled_x       = 0x7fff; // absolute mouse position, scaled from 0 to 0xffff
static uint16_t  scaled_y       = 0x7fff; // 0x7fff is a center position
static int8_t    wheel          = 0;      // wheel movement counter

static int16_t   offset_x       = 0;      // offses between host and guest mouse coordinates (in host pixels)
static int16_t   offset_y       = 0;      // (in host pixels)

bool mouse_vmware = false;             // if true, VMware compatible driver has taken over the mouse

// ***************************************************************************
// VMware interface implementation
// ***************************************************************************

static void CmdGetVersion() {
    reg_eax = 0; // TODO: should we respond with something resembling VMware? For now 0 seems OK
    reg_ebx = VMWARE_MAGIC;
}

static void CmdAbsPointerData() {
    reg_eax = buttons_vmware;
    reg_ebx = scaled_x;
    reg_ecx = scaled_y;
    reg_edx = static_cast<uint32_t>((wheel >= 0) ? wheel : 0x100 + wheel);

    wheel = 0;
}

static void CmdAbsPointerStatus() {
    reg_eax = updated ? 4 : 0;
    updated = false;
}

static void CmdAbsPointerCommand() {
    switch (reg_ebx) {
    case VMwareAbsPointer::ENABLE:
        break; // can be safely ignored
    case VMwareAbsPointer::RELATIVE:
        mouse_vmware = false;
        LOG_MSG("MOUSE (PS/2): VMware protocol disabled");
        MOUSEPS2_UpdateButtonSquish();
        GFX_UpdateMouseState();
        break;
    case VMwareAbsPointer::ABSOLUTE:
        mouse_vmware = true;
        wheel = 0;
        LOG_MSG("MOUSE (PS/2): VMware protocol enabled");
        MOUSEPS2_UpdateButtonSquish();
        GFX_UpdateMouseState();
        break;
    default:
        LOG_WARNING("MOUSE (PS/2): unimplemented VMware subcommand 0x%08x", reg_ebx);
        break;
    }
}

static uint16_t PortReadVMware(io_port_t, io_width_t) {
    if (reg_eax != VMWARE_MAGIC)
        return 0;

    switch (reg_cx) {
    case VMwareCmd::GETVERSION:         CmdGetVersion();        break;
    case VMwareCmd::ABSPOINTER_DATA:    CmdAbsPointerData();    break;
    case VMwareCmd::ABSPOINTER_STATUS:  CmdAbsPointerStatus();  break;
    case VMwareCmd::ABSPOINTER_COMMAND: CmdAbsPointerCommand(); break;
    default:
        LOG_WARNING("MOUSE (PS/2): unimplemented VMware command 0x%08x", reg_ecx);
        break;
    }

    return reg_ax;
}

bool MOUSEVMWARE_NotifyMoved(int32_t x_abs, int32_t y_abs) {
    float vmw_x, vmw_y;
    if (mouse_video.fullscreen) {
        // We have to maintain the diffs (offsets) between host and guest
        // mouse positions; otherwise in case of clipped picture (like
        // 4:3 screen displayed on 16:9 fullscreen mode) we could have
        // an effect of 'sticky' borders if the user moves mouse outside
        // of the guest display area

        if (x_abs + offset_x < mouse_video.clip_x)
                offset_x = static_cast<int16_t>(mouse_video.clip_x - x_abs);
        else if (x_abs + offset_x >= mouse_video.res_x + mouse_video.clip_x)
                offset_x = static_cast<int16_t>(mouse_video.res_x + mouse_video.clip_x - x_abs - 1);

        if (y_abs + offset_y < mouse_video.clip_y)
                offset_y = static_cast<int16_t>(mouse_video.clip_y - y_abs);
        else if (y_abs + offset_y >= mouse_video.res_y + mouse_video.clip_y)
                offset_y = static_cast<int16_t>(mouse_video.res_y + mouse_video.clip_y - y_abs - 1);

        vmw_x = static_cast<float>(x_abs + offset_x - mouse_video.clip_x);
        vmw_y = static_cast<float>(y_abs + offset_y - mouse_video.clip_y);
    }
    else {
        vmw_x = static_cast<float>(std::max(x_abs - mouse_video.clip_x, 0));
        vmw_y = static_cast<float>(std::max(y_abs - mouse_video.clip_y, 0));
    }

    auto old_x = scaled_x;
    auto old_y = scaled_y;

    scaled_x = static_cast<uint16_t>(std::min(0xffffu,
        static_cast<uint32_t>(vmw_x * 0xffff / static_cast<float>(mouse_video.res_x - 1) + 0.499)));
    scaled_y = static_cast<uint16_t>(std::min(0xffffu,
        static_cast<uint32_t>(vmw_y * 0xffff / static_cast<float>(mouse_video.res_y - 1) + 0.499)));

    updated = true;

    return mouse_vmware && (old_x != scaled_x || old_y != scaled_y);
}

bool MOUSEVMWARE_NotifyPressedReleased(uint8_t buttons_12S) {
    buttons_vmware = 0;

    if (buttons_12S & 1) buttons_vmware |= VMwareButton::LEFT;
    if (buttons_12S & 2) buttons_vmware |= VMwareButton::RIGHT;
    if (buttons_12S & 4) buttons_vmware |= VMwareButton::MIDDLE;

    updated = true;

    return mouse_vmware;
}

bool MOUSEVMWARE_NotifyWheel(int32_t w_rel) {
    if (mouse_vmware) {
        wheel   = static_cast<int8_t>(std::clamp(w_rel + wheel, INT8_MIN, INT8_MAX));
        updated = true;
    }

    return mouse_vmware;
}

void MOUSEVMWARE_NewScreenParams(int32_t x_abs, int32_t y_abs) {

    // Adjust clipping, toprevent cursor jump with the next mouse move on the host side

    offset_x = static_cast<int16_t>(std::clamp(static_cast<int32_t>(offset_x), -mouse_video.clip_x, static_cast<int32_t>(mouse_video.clip_x)));
    offset_y = static_cast<int16_t>(std::clamp(static_cast<int32_t>(offset_y), -mouse_video.clip_y, static_cast<int32_t>(mouse_video.clip_y)));

    // Report a fake mouse movement

    if (MOUSEVMWARE_NotifyMoved(x_abs, y_abs) && mouse_vmware)
        MOUSE_NotifyMovedFake();
}

void MOUSEVMWARE_Init() {
    IO_RegisterReadHandler(VMWARE_PORT, PortReadVMware, io_width_t::word, 1);
}
