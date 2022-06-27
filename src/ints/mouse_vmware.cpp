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
#include "mouse_core.h"

#include <algorithm>

#include "checks.h"
#include "regs.h"
#include "inout.h"

CHECK_NARROWING();

// VMware mouse interface passes both absolute mouse position and button
// state to the guest side driver, but still relies on PS/2 interface,
// which has to be used to listen for events

// Reference:
// - https://wiki.osdev.org/VMware_tools
// - https://wiki.osdev.org/VirtualBox_Guest_Additions (planned support)
// Drivers:
// - https://git.javispedro.com/cgit/vbados.git
// - https://github.com/NattyNarwhal/vmwmouse (warning: release 0.1 is unstable)
// - https://git.javispedro.com/cgit/vbmouse.git (planned support)

enum class VMwareCmd : uint16_t {
    GetVersion        = 10,
    AbsPointerData    = 39,
    AbsPointerStatus  = 40,
    AbsPointerCommand = 41,
};

enum class VMwareAbsPointer : uint32_t {
    Enable   = 0x45414552,
    Relative = 0xF5,
    Absolute = 0x53424152,
};

union VMwareButtons {
    uint8_t data = 0;
    bit_view<5, 1> left;
    bit_view<4, 1> right;
    bit_view<3, 1> middle;
};

static constexpr io_port_t VMWARE_PORT = 0x5658u; // communication port
// static constexpr io_port_t VMWARE_PORTHB = 0x5659u;  // communication port,
                                                        // high bandwidth
static constexpr uint32_t VMWARE_MAGIC = 0x564D5868u; // magic number for all
                                                      // VMware calls
static constexpr uint32_t ABS_UPDATED = 4;            // tells that new pointer
                                                      // position is available
static constexpr uint32_t ABS_NOT_UPDATED = 0;

static bool updated = false;  // true = mouse state update waits to be picked up
static VMwareButtons buttons; // state of mouse buttons, in VMware format
static uint16_t scaled_x = 0x7fff; // absolute mouse position, scaled from 0 to
                                   // 0xffff
static uint16_t scaled_y = 0x7fff; // 0x7fff is a center position
static int8_t wheel      = 0;      // wheel movement counter

static int16_t offset_x = 0; // offset between host and guest mouse coordinates
static int16_t offset_y = 0; // (in host pixels)

// ***************************************************************************
// VMware interface implementation
// ***************************************************************************

static void MOUSEVMM_Activate()
{
	if (!mouse_shared.active_vmm) {
		mouse_shared.active_vmm = true;
		LOG_MSG("MOUSE (PS/2): VMware protocol enabled");
		MOUSEPS2_UpdateButtonSquish();
		MOUSE_NotifyStateChanged();
	}
	buttons.data = 0;
	wheel = 0;
}

void MOUSEVMM_Deactivate()
{
	if (mouse_shared.active_vmm) {
		mouse_shared.active_vmm = false;
		LOG_MSG("MOUSE (PS/2): VMware protocol disabled");
		MOUSEPS2_UpdateButtonSquish();
		MOUSE_NotifyStateChanged();
	}
	buttons.data = 0;
	wheel = 0;
}

static void CmdGetVersion()
{
    reg_eax = 0; // protocol version (TODO: is it OK?)
    reg_ebx = VMWARE_MAGIC;
}

static void CmdAbsPointerData()
{
    reg_eax = buttons.data;
    reg_ebx = scaled_x;
    reg_ecx = scaled_y;
    reg_edx = static_cast<uint32_t>((wheel >= 0) ? wheel : 0x100 + wheel);

    wheel = 0;
}

static void CmdAbsPointerStatus()
{
    reg_eax = updated ? ABS_UPDATED : ABS_NOT_UPDATED;
    updated = false;
}

static void CmdAbsPointerCommand()
{
    switch (static_cast<VMwareAbsPointer>(reg_ebx)) {
    case VMwareAbsPointer::Enable: break; // can be safely ignored
    case VMwareAbsPointer::Relative:
		MOUSEVMM_Deactivate();
        break;
    case VMwareAbsPointer::Absolute:
	    MOUSEVMM_Activate();
        break;
    default:
        LOG_WARNING("MOUSE (PS/2): unimplemented VMware subcommand 0x%08x", reg_ebx);
        break;
    }
}

static uint32_t PortReadVMware(const io_port_t, const io_width_t)
{
    if (reg_eax != VMWARE_MAGIC)
        return 0;

    switch (static_cast<VMwareCmd>(reg_cx)) {
    case VMwareCmd::GetVersion: CmdGetVersion(); break;
    case VMwareCmd::AbsPointerData: CmdAbsPointerData(); break;
    case VMwareCmd::AbsPointerStatus: CmdAbsPointerStatus(); break;
    case VMwareCmd::AbsPointerCommand: CmdAbsPointerCommand(); break;
    default:
        LOG_WARNING("MOUSE (PS/2): unimplemented VMware command 0x%08x",
                    reg_ecx);
        break;
    }

    return reg_eax;
}

bool MOUSEVMM_NotifyMoved(const uint16_t x_abs, const uint16_t y_abs)
{
    auto calculate = [](const uint16_t absolute,
                        int16_t &offset,
                        const uint16_t res,
                        const uint16_t clip) {
        float unscaled; // unscaled guest mouse coordinate
        if (mouse_video.fullscreen) {
            // We have to maintain the diffs (offsets) between host
            // and guest mouse positions; otherwise in case of
            // clipped picture (like 4:3 screen displayed on 16:9
            // fullscreen mode) we could have an effect of 'sticky'
            // borders if the user moves mouse outside of the guest
            // display area

            // Guest mouse position is a host mouse position +
            // offset, which is 0 at the beginning. Once the guest
            // mouse cursor is at the edge of the screen, and the
            // host mouse cursor continues moving outside, the
            // offset is increased or decreased to accomodate
            // changes. Once the host mouse cursor starts moving
            // back, we continue with the same offset, so that guest
            // mouse cursor starts moving immediately.

            if (absolute + offset < clip)
                offset = static_cast<int16_t>(clip - absolute);
            else if (absolute + offset >= res + clip)
                offset = static_cast<int16_t>(res + clip - absolute - 1);

            unscaled = static_cast<float>(absolute + offset - clip);
        } else {
            // Skip the offset mechanism if not in fullscreen mode
            unscaled = static_cast<float>(std::max(absolute - clip, 0));

            // TODO: If the pointer go out of guest screen (it is in the
            // black border area), show host mouse cursor
        }

        assert(res > 1u);
        const auto scale = static_cast<float>(UINT16_MAX) /
                           static_cast<float>(res - 1);
        const auto tmp = std::min(static_cast<uint32_t>(UINT16_MAX),
                                  static_cast<uint32_t>(unscaled * scale + 0.499f));
        return static_cast<uint16_t>(tmp);
    };

    const auto old_x = scaled_x;
    const auto old_y = scaled_y;

    scaled_x = calculate(x_abs, offset_x, mouse_video.res_x, mouse_video.clip_x);
    scaled_y = calculate(y_abs, offset_y, mouse_video.res_y, mouse_video.clip_y);

    updated = true;

    // Filter out unneeded events (like sub-pixel mouse movements,
    // which won't change guest side mouse state)
    return (old_x != scaled_x || old_y != scaled_y);
}

bool MOUSEVMM_NotifyPressedReleased(const MouseButtons12S buttons_12S)
{
    if (!mouse_shared.active_vmm) return false;

    buttons.data = 0;

    // Direct assignment of .data is not possible, as bit layout is different
    buttons.left   = (int) buttons_12S.left;
    buttons.right  = (int) buttons_12S.right;
    buttons.middle = (int) buttons_12S.middle;

    updated = true;

    return true;
}

bool MOUSEVMM_NotifyWheel(const int16_t w_rel)
{
    if (!mouse_shared.active_vmm) return false;

    const auto tmp = std::clamp(static_cast<int32_t>(w_rel + wheel),
                                static_cast<int32_t>(INT8_MIN),
                                static_cast<int32_t>(INT8_MAX));
    wheel   = static_cast<int8_t>(tmp);
    updated = true;

    return true;
}

void MOUSEVMM_NewScreenParams(const uint16_t x_abs, const uint16_t y_abs)
{
    // Adjust offset, to prevent cursor jump with the next mouse move on the
    // host side

    auto ClampOffset = [](const int16_t offset, uint16_t clip) {
        const auto tmp = std::clamp(static_cast<int32_t>(offset),
                                    static_cast<int32_t>(-clip),
                                    static_cast<int32_t>(clip));
        return static_cast<int16_t>(tmp);
    };

    offset_x = ClampOffset(offset_x, mouse_video.clip_x);
    offset_y = ClampOffset(offset_y, mouse_video.clip_y);

    // Report a fake mouse movement

    if (MOUSEVMM_NotifyMoved(x_abs, y_abs) && mouse_shared.active_vmm)
        MOUSE_NotifyMovedFake();
}

void MOUSEVMM_Init()
{
    IO_RegisterReadHandler(VMWARE_PORT, PortReadVMware, io_width_t::dword);
}
