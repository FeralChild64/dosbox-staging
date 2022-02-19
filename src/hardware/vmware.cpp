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
// Mouse driver tested using unofficial Windows 3.1 driver https://github.com/NattyNarwhal/vmwmouse

static constexpr Bit32u VMWARE_MAGIC           = 0x564D5868u;    // magic number for all VMware calls
static constexpr Bit32u VMWARE_PORT            = 0x5658u;        // communication port
static constexpr Bit32u VMWARE_PORTHB          = 0x5659u;        // communication port, high bandwidth

static constexpr Bit32u CMD_GETVERSION         = 10u;
static constexpr Bit32u CMD_ABSPOINTER_DATA    = 39u;
static constexpr Bit32u CMD_ABSPOINTER_STATUS  = 40u;
static constexpr Bit32u CMD_ABSPOINTER_COMMAND = 41u;

static constexpr Bit32u ABSPOINTER_ENABLE      = 0x45414552u;
static constexpr Bit32u ABSPOINTER_RELATIVE    = 0xF5u;
static constexpr Bit32u ABSPOINTER_ABSOLUTE    = 0x53424152u;

static constexpr Bit8u  BUTTON_LEFT            = 0x20u;
static constexpr Bit8u  BUTTON_RIGHT           = 0x10u;
static constexpr Bit8u  BUTTON_MIDDLE          = 0x08u;

class Section;

class VMware final {

private:

	IO_ReadHandleObject ReadHandler = {};

	static Bit8u  mouseButtons;   // state of mouse buttons, in VMware format
	static Bit16u mousePosX;      // mouse position X, in VMware format (scaled from 0 to 0xFFFF)
	static Bit16u mousePosY;      // ditto

	static void CmdGetVersion() {
		reg_eax = 0; // FIXME: respond with something resembling VMware
		reg_ebx = VMWARE_MAGIC;
	}

	static void AbsPointerData() {
		reg_eax = mouseButtons;
		reg_ebx = mousePosX;
		reg_ecx = mousePosY;
		reg_edx = 0; // FIXME: implement scroll wheel
	}

	static void AbsPointerStatus() {
		reg_eax = 4; // XXX do it only if there is a fake mouse event waiting
	}

	static void AbsPointerCommand() {
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
				// XXX from now on, only send fake events to PS/2 queue
				// XXX implement Mouse_DumyEvent();
				break;
			default:
				LOG_WARNING("VMWARE: unknown mouse subcommand 0x%08x", reg_ebx);
				break;
		}
	}

	static Bit16u PortRead(io_port_t, io_width_t) {
		if (reg_eax != VMWARE_MAGIC) {
			return 0;
		}

		// LOG_MSG("VMWARE: called with EBX 0x%08x, ECX 0x%08x", reg_ebx, reg_ecx);

		switch (reg_ecx) {
			case CMD_GETVERSION:
				CmdGetVersion();
				break;
			case CMD_ABSPOINTER_DATA:
				AbsPointerData();
				break;
			case CMD_ABSPOINTER_STATUS:
				AbsPointerStatus();
				break;
			case CMD_ABSPOINTER_COMMAND:
				AbsPointerCommand();
				break;
			default:
				LOG_WARNING("VMWARE: unknown command 0x%08x", reg_ecx);
				break;
		}

		return reg_ax;
	}

public:

	VMware() {
		vmware_mouse = false;
		ReadHandler.Install(static_cast<io_port_t>(VMWARE_PORT), PortRead, io_width_t::word);
	}

	void MouseButtonPressed(Bit8u button) {
		switch (button) {
			case 0:
				mouseButtons |= BUTTON_LEFT;
				break;
			case 1:
				mouseButtons |= BUTTON_RIGHT;
				break;
			case 2:
				mouseButtons |= BUTTON_MIDDLE;
				break;
			default:
				break;
		}
	}

	void MouseButtonReleased(Bit8u button)  {
		switch (button) {
			case 0:
				mouseButtons &= ~BUTTON_LEFT;
				break;
			case 1:
				mouseButtons &= ~BUTTON_RIGHT;
				break;
			case 2:
				mouseButtons &= ~BUTTON_MIDDLE;
				break;
			default:
				break;
		}
	}

	void MousePosition(Bit32u posX, Bit32u posY, Bit32u resX, Bit32u resY) {
		mousePosX = std::min(0xFFFFu, static_cast<unsigned int>(static_cast<float>(posX) / (resX - 1) * 0xFFFF + 0.499));
		mousePosY = std::min(0xFFFFu, static_cast<unsigned int>(static_cast<float>(posY) / (resY - 1) * 0xFFFF + 0.499));
	}
};

Bit8u  VMware::mouseButtons   = 0;
Bit16u VMware::mousePosX      = 0x8000;
Bit16u VMware::mousePosY      = 0x8000;

volatile bool vmware_mouse    = false;

static VMware *vmware         = nullptr;

void VMWARE_Init(Section *) {
	delete vmware;
	vmware = new VMware();
}

void VMWARE_ShutDown(Section *) {
	delete vmware;
	vmware = nullptr;
}

void VMware_MouseButtonPressed(Bit8u button)
{
	if (vmware)
		vmware->MouseButtonPressed(button);
}

void VMware_MouseButtonReleased(Bit8u button)
{
	if (vmware)
		vmware->MouseButtonReleased(button);
}

void VMware_MousePosition(Bit32u posX, Bit32u posY, Bit32u resX, Bit32u resY)
{
	if (vmware)
	{
		vmware->MousePosition(posX, posY, resX, resY);
	}
}
