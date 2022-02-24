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

volatile bool vmware_mouse     = false;  // if true, VMware compatible driver has taken over the mouse

static Bit8u  mouse_buttons    = 0;      // state of mouse buttons, in VMware format
static Bit16u mouse_x          = 0x8000; // mouse position X, in VMware format (scaled from 0 to 0xFFFF)
static Bit16u mouse_y          = 0x8000; // ditto
static Bit8s  mouse_wheel      = 0;
static bool   mouse_updated    = false;

static Bit16s mouse_diff_x     = 0;      // difference between host and guest mouse x coordinate (in host pixels)
static Bit16s mouse_diff_y     = 0;      // ditto

static bool   video_fullscreen = false;
static Bit16u video_res_x      = 1;      // resolution to which guest image is scaled, excluding black borders
static Bit16u video_res_y      = 1;
static Bit16u video_clip_x     = 0;      // clipping value - size of black border (one side)
static Bit16u video_clip_y     = 0;


class Section;

// Commands (requests) to the VMware hypervisor

static inline void CmdGetVersion() {

        reg_eax = 0; // FIXME: should we respond with something resembling VMware?
        reg_ebx = VMWARE_MAGIC;
}

static inline void CmdAbsPointerData() {

        reg_eax = mouse_buttons;
        reg_ebx = mouse_x;
        reg_ecx = mouse_y;
        reg_edx = (mouse_wheel >= 0) ? mouse_wheel : 256 + mouse_wheel;

        mouse_wheel = 0;
}

static inline void CmdAbsPointerStatus() {

        reg_eax = mouse_updated ? 4 : 0;
        mouse_updated = false;
}

static inline void CmdAbsPointerCommand() {

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

static Bit16u PortRead(io_port_t, io_width_t) {

        if (reg_eax != VMWARE_MAGIC)
                return 0;

        // LOG_MSG("VMWARE: called with EBX 0x%08x, ECX 0x%08x", reg_ebx, reg_ecx);

        switch (reg_cx) {
        case CMD_GETVERSION:
                CmdGetVersion();
                break;
        case CMD_ABSPOINTER_DATA:
                CmdAbsPointerData();
                break;
        case CMD_ABSPOINTER_STATUS:
                CmdAbsPointerStatus();
                break;
        case CMD_ABSPOINTER_COMMAND:
                CmdAbsPointerCommand();
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

void VMWARE_MousePosition(Bit16u pos_x,  Bit16u pos_y) {
     
        float tmp_x;
        float tmp_y;

        if (video_fullscreen)
        {
                // We have to maintain the diffs (offsets) between host and guest
                // mouse positions; otherwise in case of clipped picture (like
                // 4:3 screen displayed on 16:9 fullscreen mode) we could have
                // an effect of 'sticky' borders if the user moves mouse outside
                // of the guest display area

                if (pos_x + mouse_diff_x < video_clip_x)
                        mouse_diff_x = video_clip_x - pos_x;
                else if (pos_x + mouse_diff_x >= video_res_x + video_clip_x)
                        mouse_diff_x = video_res_x + video_clip_x - pos_x - 1;

                if (pos_y + mouse_diff_y < video_clip_y)
                        mouse_diff_y = video_clip_y - pos_y;
                else if (pos_y + mouse_diff_y >= video_res_y + video_clip_y)
                        mouse_diff_y = video_res_y + video_clip_y - pos_y - 1;

                tmp_x = pos_x + mouse_diff_x - video_clip_x;
                tmp_y = pos_y + mouse_diff_y - video_clip_y;
        }
        else
        {
                tmp_x = std::max(pos_x - video_clip_x, 0);
                tmp_y = std::max(pos_y - video_clip_y, 0);
        }

        mouse_x = std::min(0xFFFFu, static_cast<Bit32u>(tmp_x * 0xFFFF / (video_res_x - 1) + 0.499));
        mouse_y = std::min(0xFFFFu, static_cast<Bit32u>(tmp_y * 0xFFFF / (video_res_y - 1) + 0.499));

        mouse_updated = true;
}

void VMWARE_MouseWheel(Bit32s scroll) {

        if (scroll >= 255 || scroll + mouse_wheel >= 127)
                mouse_wheel = 127;
        else if (scroll <= -255 || scroll + mouse_wheel <= -127)
                mouse_wheel = -127; // protocol limit is -128,127 - but let's keep it symmetric
        else
                mouse_wheel += scroll;

        mouse_updated = true;       
}

void VMWARE_ScreenParams(Bit16u clip_x, Bit16u clip_y,
                         Bit16u res_x,  Bit16u res_y,
                         bool fullscreen) {

        video_clip_x     = clip_x;
        video_clip_y     = clip_y;
        video_res_x      = res_x;
        video_res_y      = res_y;
        video_fullscreen = fullscreen;

        // Unfortunately, with seamless driver changing the window size can cause
        // mouse movement as a side-effect, this is not fun for games. Let's try
        // to at least minimize the effect.

        mouse_diff_x = std::clamp(static_cast<Bit32s>(mouse_diff_x), -video_clip_x, static_cast<Bit32s>(video_clip_x));
        mouse_diff_y = std::clamp(static_cast<Bit32s>(mouse_diff_y), -video_clip_y, static_cast<Bit32s>(video_clip_y));
}

// Lifecycle

void VMWARE_Init(Section *) {
        IO_RegisterReadHandler(VMWARE_PORT, PortRead, io_width_t::word, 1);
}
