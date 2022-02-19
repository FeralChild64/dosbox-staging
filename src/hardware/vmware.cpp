/*
 *  Copyright (C) 2022  The DOSBox Staging Team
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

#include "vmware.h"

#include "inout.h"
#include "regs.h"
#include "video.h"

// Basic VMware tools support, based on documentation from https://wiki.osdev.org/VMware_tools
// Mouse support tested using unofficial Windows 3.1 driver from https://github.com/NattyNarwhal/vmwmouse

static constexpr io_port_t VMWARE_PORT         = 0x5658u;        // communication port
static constexpr io_port_t VMWARE_PORTHB       = 0x5659u;        // communication port, high bandwidth

static constexpr Bit32u VMWARE_MAGIC           = 0x564D5868u;    // magic number for all VMware calls

static constexpr Bit16u CMD_GETVERSION         = 10u;
static constexpr Bit16u CMD_ABSPOINTER_DATA    = 39u;
static constexpr Bit16u CMD_ABSPOINTER_STATUS  = 40u;
static constexpr Bit16u CMD_ABSPOINTER_COMMAND = 41u;

static constexpr Bit32u ABSPOINTER_ENABLE      = 0x45414552u;
static constexpr Bit32u ABSPOINTER_RELATIVE    = 0xF5u;
static constexpr Bit32u ABSPOINTER_ABSOLUTE    = 0x53424152u;

static constexpr Bit8u  BUTTON_LEFT            = 0x20u;
static constexpr Bit8u  BUTTON_RIGHT           = 0x10u;
static constexpr Bit8u  BUTTON_MIDDLE          = 0x08u;

volatile bool vmware_mouse  = false;  // if true, VMware compatible driver has taken over the mouse

static Bit8u  mouse_buttons = 0;      // state of mouse buttons, in VMware format
static Bit16u mouse_x       = 0x8000; // mouse position X, in VMware format (scaled from 0 to 0xFFFF)
static Bit16u mouse_y       = 0x8000; // ditto
static Bit8s  mouse_wheel   = 0;
static bool   mouse_updated = false;

class Section;

// Commands (requests) to the VMware hypervisor

static void VMWARE_CmdGetVersion() {

        reg_eax = 0; // FIXME: should we respond with something resembling VMware?
        reg_ebx = VMWARE_MAGIC;
}

static void VMWARE_CmdAbsPointerData() {

        reg_eax = mouse_buttons;
        reg_ebx = mouse_x;
        reg_ecx = mouse_y;
        reg_edx = (mouse_wheel >= 0) ? mouse_wheel : 256 + mouse_wheel;

        mouse_wheel = 0;
}

static void VMWARE_CmdAbsPointerStatus() {

        reg_eax = mouse_updated ? 4 : 0;
        mouse_updated = false;
}

static void VMWARE_CmdAbsPointerCommand() {

        switch (reg_ebx) {
        case ABSPOINTER_ENABLE:
                // can be safely ignored
                break;
        case ABSPOINTER_RELATIVE:
                vmware_mouse = false;
                GFX_UpdateMouseState();
                break;
        case ABSPOINTER_ABSOLUTE:
                vmware_mouse = true;
                GFX_UpdateMouseState();
                break;
        default:
                LOG_WARNING("VMWARE: unknown mouse subcommand 0x%08x", reg_ebx);
                break;
        }
}

// IO port handling

static Bit16u VMWARE_PortRead(io_port_t, io_width_t) {

        if (reg_eax != VMWARE_MAGIC)
                return 0;

        // LOG_MSG("VMWARE: called with EBX 0x%08x, ECX 0x%08x", reg_ebx, reg_ecx);

        switch (reg_cx) {
        case CMD_GETVERSION:
                VMWARE_CmdGetVersion();
                break;
        case CMD_ABSPOINTER_DATA:
                VMWARE_CmdAbsPointerData();
                break;
        case CMD_ABSPOINTER_STATUS:
                VMWARE_CmdAbsPointerStatus();
                break;
        case CMD_ABSPOINTER_COMMAND:
                VMWARE_CmdAbsPointerCommand();
                break;
        default:
                LOG_WARNING("VMWARE: unknown command 0x%08x", reg_ecx);
                break;
        }

        return reg_ax;
}

// Notifications from external subsystems

void VMWARE_MouseButtonPressed(Bit8u button) {

        switch (button) {
        case 0:
                mouse_buttons |= BUTTON_LEFT;
                mouse_updated = true;
                break;
        case 1:
                mouse_buttons |= BUTTON_RIGHT;
                mouse_updated = true;
                break;
        case 2:
                mouse_buttons |= BUTTON_MIDDLE;
                mouse_updated = true;
                break;
        default:
                break;
        }
}

void VMWARE_MouseButtonReleased(Bit8u button) {

        switch (button) {
        case 0:
                mouse_buttons &= ~BUTTON_LEFT;
                mouse_updated = true;
                break;
        case 1:
                mouse_buttons &= ~BUTTON_RIGHT;
                mouse_updated = true;
                break;
        case 2:
                mouse_buttons &= ~BUTTON_MIDDLE;
                mouse_updated = true;
                break;
        default:
                break;
        }
}

void VMWARE_MousePosition(Bit32u pos_x, Bit32u pos_y, Bit32u res_x, Bit32u res_y) {

        mouse_x = std::min(0xFFFFu, static_cast<unsigned int>(static_cast<float>(pos_x) / (res_x - 1) * 0xFFFF + 0.499));
        mouse_y = std::min(0xFFFFu, static_cast<unsigned int>(static_cast<float>(pos_y) / (res_y - 1) * 0xFFFF + 0.499));
        mouse_updated = true;
}

void VMWARE_MouseWheel(Bit32s scroll) {

        if (scroll >= 255 || scroll + mouse_wheel >= 127)
                mouse_wheel = 127;
        else if (scroll <= -255 || scroll + mouse_wheel <= -127)
                mouse_wheel = -127;
        else
                mouse_wheel += scroll;

        mouse_updated = true;       
}

// Lifecycle

void VMWARE_Init(Section *) {

        IO_RegisterReadHandler(VMWARE_PORT, VMWARE_PortRead, io_width_t::word, 1);
}
