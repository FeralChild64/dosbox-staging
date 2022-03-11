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

// Microsoft Serial Mouse compatible emulation
// Originally wrtitten by Jonathan Campbell

#include "serialmouse.h"

#include "mouse.h"

CSerialMouse::CSerialMouse(Bitu id, CommandLine* cmd): CSerial(id, cmd),
    send_ack(true),
    packet_xmit(0xff),
    xmit_another_packet(false),
    mouse_buttons(0),
    mouse_delta_x(0),
    mouse_delta_y(0),
    mouse_delta_w(0)
{
    CSerial::Init_Registers();
    setRI(false);
    setDSR(false);
    setCD(false);
    setCTS(false);
    InstallationSuccessful = true;

    MouseSERIAL_Register(this);
}

CSerialMouse::~CSerialMouse() {
    MouseSERIAL_UnRegister(this);
    removeEvent(SERIAL_TX_EVENT); // clear events
}

void CSerialMouse::onMouseReset() {
    send_ack            = true;
    packet_xmit         = 0xff;
    xmit_another_packet = false;
    mouse_buttons       = 0;
    mouse_delta_x       = 0;
    mouse_delta_y       = 0;
    mouse_delta_w       = 0;

    setEvent(SERIAL_RX_EVENT, bytetime);
}

void CSerialMouse::onMouseEvent(Bit16s delta_x, Bit16s delta_y, Bit16s delta_w, Bit8u buttons) {
    mouse_buttons  = buttons & 7;
    mouse_delta_x += delta_x;
    mouse_delta_y += delta_y;
    mouse_delta_w += delta_w;

    // initiate data transfer and form the packet to transmit. if another packet
    // is already transmitting now then wait for it to finish before transmitting ours,
    // and let the mouse motion accumulate in the meantime

    if (packet_xmit >= 3)
        startPacket();
    else
        xmit_another_packet = true;
}

void CSerialMouse::startPacket() {
    xmit_another_packet = false;

    // API limits are -128,127 - but let's keep it symmetric
    mouse_delta_x = std::clamp(mouse_delta_x, -127, 127) & 0xff;
    mouse_delta_y = std::clamp(mouse_delta_y, -127, 127) & 0xff;

    // Do NOT set bit 7. It confuses CTMOUSE.EXE (CuteMouse) serial support.
    // Leaving it clear is the only way to make mouse movement possible.
    // Microsoft Windows on the other hand doesn't care if bit 7 is set.

    packet_xmit = 0;
    /* Byte 0:    X   1  LB  RB  Y7  Y6  X7  X6 */
    packet[0] = 0x40 | ((mouse_buttons & 1) << 5) | ((mouse_buttons & 2) << 3) |
                       (((mouse_delta_y >> 6) & 3) << 2) | ((mouse_delta_x >> 6) & 3);
    /* Byte 1:    X   0  X5-X0 */
    packet[1] = 0x00 | (mouse_delta_x & 0x3F);
    /* Byte 2:    X   0  Y5-Y0 */
    packet[2] = 0x00 | (mouse_delta_y & 0x3F);

    // clear counters
    mouse_delta_x = 0;
    mouse_delta_y = 0;
    mouse_delta_w = 0;

    setEvent(SERIAL_RX_EVENT, bytetime);
}

void CSerialMouse::handleUpperEvent(Bit16u type) {
    if (type == SERIAL_TX_EVENT) {
        ByteTransmitted(); // tx timeout
    } else if (type == SERIAL_THR_EVENT) {
        ByteTransmitting();
        setEvent(SERIAL_TX_EVENT, bytetime);
    }
    else if (type == SERIAL_RX_EVENT) {
        // check for bytes to be sent to port
        if (CSerial::CanReceiveByte()) {
            if (send_ack) {
                send_ack = false;
                CSerial::receiveByte('M');
                setEvent(SERIAL_RX_EVENT, bytetime);
            } else if (packet_xmit < 3) {
                CSerial::receiveByte(packet[packet_xmit++]);
                if (packet_xmit >= 3 && xmit_another_packet)
                    startPacket();
                else
                    setEvent(SERIAL_RX_EVENT, bytetime);
            }
        } else {
            setEvent(SERIAL_RX_EVENT, bytetime);
        }
    }
}

void CSerialMouse::updatePortConfig(uint16_t, uint8_t) {
}

void CSerialMouse::updateMSR() {
}

void CSerialMouse::transmitByte(uint8_t, bool first) {
    if (first)
        setEvent(SERIAL_THR_EVENT, bytetime / 10); 
    else
        setEvent(SERIAL_TX_EVENT, bytetime);
}

void CSerialMouse::setBreak(bool) {
}

void CSerialMouse::setRTSDTR(bool rts, bool dtr) {
    if (rts && dtr && !getRTS() && !getDTR()) {
        // The serial mouse driver turns on the mouse by bringing up
        // RTS and DTR. Not just for show, but to give the serial mouse
        // a power source to work from. Likewise, drivers "reset" the
        // mouse by bringing down the lines, then bringing them back
        // up. And most drivers turn off the mouse when not in use by
        // bringing them back down and leaving them that way.
        //
        // We're expected to transmit ASCII character 'M' when first
        // initialized, so that the driver knows we're a Microsoft
        // compatible serial mouse attached to a COM port.
        onMouseReset();
    }

    setRTS(rts);
    setDTR(dtr);
}

void CSerialMouse::setRTS(bool val) {
    if (val && !getRTS() && getDTR()) {
        onMouseReset();
    }

    setCTS(val);
}
void CSerialMouse::setDTR(bool val) {
    if (val && !getDTR() && getRTS()) {
        onMouseReset();
    }

    setDSR(val);
    setRI(val);
    setCD(val);
}
