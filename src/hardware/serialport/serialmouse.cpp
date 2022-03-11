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

// Microsoft Serial Mouse emulation originally wrtitten by Jonathan Campbell
// Wheel, Logitech, and Mouse Systems mice added by Roman Standzikowski

// Reference:
// - https://roborooter.com/post/serial-mice
// - https://www.cpcwiki.eu/index.php/Serial_RS232_Mouse

#include "serialmouse.h"

#include "mouse.h"

CSerialMouse::CSerialMouse(Bitu id, CommandLine* cmd): CSerial(id, cmd),
    config_type(MouseType::NONE),
    config_auto(false),
    mouse_type(MouseType::NONE),
    mouse_bytelen(0),
    mouse_has_3rd_button(false),
    mouse_has_wheel(false),
    mouse_port_valid(false),
    send_ack(true),
    packet_len(0),
    xmit_idx(0xff),
    xmit_2part(false),
    xmit_another_move(false),
    xmit_another_button(false),
    mouse_buttons(0),
    mouse_delta_x(0),
    mouse_delta_y(0),
    mouse_delta_w(0)
{
    std::string type_string;
    bool use_default = !cmd->FindStringBegin("type:", type_string, false);

    if (type_string == "msft") {
        config_type = MouseType::MICROSOFT;
        config_auto = false;
    } else if (type_string == "msft+msm") {
        config_type = MouseType::MICROSOFT;
        config_auto = true;
    } else if (type_string == "logi") {
        config_type = MouseType::LOGITECH;
        config_auto = false;
    } else if (type_string == "logi+msm") {
        config_type = MouseType::LOGITECH;
        config_auto = true;
    } else if (type_string == "wheel") {
        config_type = MouseType::WHEEL;
        config_auto = false;
    } else if (type_string == "wheel+msm" || use_default) {
        config_type = MouseType::WHEEL;
        config_auto = true;
    } else if (type_string == "msm") {
        config_type = MouseType::MOUSE_SYSTEMS;
        config_auto = false;
    } else {
        LOG_ERR("Invalid serial mouse type '%s'", type_string.c_str());
        return; // invalid type
    }

    applyType(config_type);

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

void CSerialMouse::applyType(MouseType type) {
    mouse_type = type;
    switch (type) {
    case MouseType::MICROSOFT:
        mouse_bytelen        = 7;
        mouse_has_3rd_button = false;
        mouse_has_wheel      = false;
        break;
    case MouseType::LOGITECH:
        mouse_bytelen        = 7;
        mouse_has_3rd_button = true;
        mouse_has_wheel      = false;
        break;
    case MouseType::WHEEL:
        mouse_bytelen        = 7;
        mouse_has_3rd_button = true;
        mouse_has_wheel      = true;
        break;
    case MouseType::MOUSE_SYSTEMS:
        mouse_bytelen        = 8;
        mouse_has_3rd_button = true;
        mouse_has_wheel      = false;
        break;
    default:
        unimplemented();
        break;
    }
}

void CSerialMouse::abortPacket() {
    packet_len          = 0;
    xmit_idx            = 0xff;
    xmit_2part          = false;
    xmit_another_move   = false;
    xmit_another_button = false;
}

void CSerialMouse::clearCounters() {
    mouse_delta_x       = 0;
    mouse_delta_y       = 0;
    mouse_delta_w       = 0;  
}

void CSerialMouse::mouseReset() {
    abortPacket();
    clearCounters();
    send_ack = true;

    setEvent(SERIAL_RX_EVENT, bytetime);
}

void CSerialMouse::onMouseEventMoved(Bit16s delta_x, Bit16s delta_y) {
    mouse_delta_x += delta_x;
    mouse_delta_y += delta_y;

    // initiate data transfer and form the packet to transmit. if another packet
    // is already transmitting now then wait for it to finish before transmitting ours,
    // and let the mouse motion accumulate in the meantime

    if (xmit_idx >= packet_len)
        startPacketData();
    else
        xmit_another_move = true;
}

void CSerialMouse::onMouseEventButtons(Bit8u buttons) {
    mouse_buttons = buttons;

    if (xmit_idx >= packet_len)
        startPacketData();
    else
        xmit_another_button = true;
}

void CSerialMouse::onMouseEventButton3(Bit8u buttons) {
    mouse_buttons = buttons;

    if (!mouse_has_3rd_button) return;

    if (xmit_idx >= packet_len)
        startPacketData(true);
    else
        xmit_another_button = true;
}

void CSerialMouse::onMouseEventWheel(Bit8s delta_w) {
    mouse_delta_w += delta_w;

    if (!mouse_has_wheel) return;

    if (xmit_idx >= packet_len)
        startPacketData(true);
    else
        xmit_another_button = true;
}

void CSerialMouse::startPacketId() { // send the mouse identifier
    if (!mouse_port_valid) return;
    abortPacket();
    clearCounters();

    switch (mouse_type) {
    case MouseType::MICROSOFT:
        packet[0]  = 'M';
        packet_len = 1;
        break;
    case MouseType::LOGITECH:
        packet[0]  = 'M';
        packet[1]  = '3';
        packet_len = 2;
        break;
    case MouseType::WHEEL:
        packet[0]  = 'M';
        packet[1]  = 'Z';
        packet[2]  = '@'; // for some reason 86Box sends more than 'MZ'
        packet[3]  = 0;
        packet[4]  = 0;
        packet[5]  = 0;
        packet_len = 6;
        break;
    case MouseType::MOUSE_SYSTEMS:
        packet[0]  = 'H';
        packet_len = 1;
        break;
    default:
        unimplemented();
        break;
    }

    // send packet
    xmit_idx = 0;
    setEvent(SERIAL_RX_EVENT, bytetime);
}

void CSerialMouse::startPacketData(bool extended) {
    if (!mouse_port_valid) return;

    if (mouse_type == MouseType::MICROSOFT ||
        mouse_type == MouseType::LOGITECH  ||
        mouse_type == MouseType::WHEEL) {
        //          -- -- -- -- -- -- -- --
        // Byte 0:   X  1 LB RB Y7 Y6 X7 X6
        // Byte 1:   X  0 X5 X4 X3 X2 X1 X0
        // Byte 2:   X  0 Y5 Y4 Y3 Y2 Y1 Y0
        // Byte 3:   X  0 MB 00 W3 W2 W1 W0  - only sent if needed

        // Do NOT set bit 7. It confuses CTMOUSE.EXE (CuteMouse) serial support.
        // Leaving it clear is the only way to make mouse movement possible.
        // Microsoft Windows on the other hand doesn't care if bit 7 is set.

        Bit8u dx = std::clamp(mouse_delta_x, -0x80, 0x7f) & 0xff;
        Bit8u dy = std::clamp(mouse_delta_y, -0x80, 0x7f) & 0xff;
        Bit8u bt = mouse_has_3rd_button ? (mouse_buttons & 7) : (mouse_buttons & 3);

        packet[0]  = 0x40 | ((bt & 1) << 5) | ((bt & 2) << 3) | (((dy >> 6) & 3) << 2) | ((dx >> 6) & 3);
        packet[1]  = 0x00 | (dx & 0x3f);
        packet[2]  = 0x00 | (dy & 0x3f);
        if (extended) {
            Bit8u dw   = std::clamp(mouse_delta_w, -0x10, 0x0f) & 0x0f;
            packet[3]  = ((bt & 4) ? 0x20 : 0) | dw;
            packet_len = 4;
        } else {
            packet_len = 3;           
        }
        xmit_2part = false;

    } else if (mouse_type == MouseType::MOUSE_SYSTEMS) {
        //          -- -- -- -- -- -- -- --
        // Byte 0:   1  0  0  0  0 LB MB RB
        // Byte 1:  X7 X6 X5 X4 X3 X2 X1 X0
        // Byte 2:  Y7 Y6 Y5 Y4 Y3 Y2 Y1 Y0

        Bit8u dx = std::clamp(mouse_delta_x,  -0x80, 0x7f) & 0xff;
        Bit8u dy = std::clamp(-mouse_delta_y, -0x80, 0x7f) & 0xff;
        Bit8u bt = mouse_has_3rd_button ? ((~mouse_buttons) & 7) : ((~mouse_buttons) & 3);

        packet[0]  = 0x80 | ((bt & 1) << 2) | ((bt & 2) >> 1) | ((bt & 4) >> 1);
        packet[1]  = dx;
        packet[2]  = dy;
        packet_len = 3;
        xmit_2part = true; // next part contains mouse movement since the start of the 1st part

    } else {
        unimplemented();
    }

    clearCounters();

    // send packet
    xmit_idx            = 0;
    xmit_another_button = false;
    xmit_another_move   = false;
    setEvent(SERIAL_RX_EVENT, bytetime);
}

void CSerialMouse::startPacketData2() {
    // port settings are valid at this point

    if (mouse_type == MouseType::MOUSE_SYSTEMS) {
        //          -- -- -- -- -- -- -- --
        // Byte 3:  X7 X6 X5 X4 X3 X2 X1 X0
        // Byte 4:  Y7 Y6 Y5 Y4 Y3 Y2 Y1 Y0

        Bit8u dx = std::clamp(mouse_delta_x,  -0x80, 0x7f) & 0xff;
        Bit8u dy = std::clamp(-mouse_delta_y, -0x80, 0x7f) & 0xff;

        packet[0]  = dx;
        packet[1]  = dy;

        packet_len = 2;
        xmit_2part = false;
    } else {
        unimplemented();
    }

    clearCounters();

    // send packet
    xmit_idx            = 0;
    xmit_another_move   = false;
    setEvent(SERIAL_RX_EVENT, bytetime);
}

void CSerialMouse::unimplemented() {
    LOG_ERR("Missing implementation in serial mouse");
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
                startPacketId();
            } else if (xmit_idx < packet_len) {
                CSerial::receiveByte(packet[xmit_idx++]);
                if (xmit_idx >= packet_len && xmit_2part)
                    startPacketData2();
                else if (xmit_idx >= packet_len && (xmit_another_move || xmit_another_button))
                    startPacketData();
                else
                    setEvent(SERIAL_RX_EVENT, bytetime);
            }
        } else {
            setEvent(SERIAL_RX_EVENT, bytetime);
        }
    }
}

void CSerialMouse::updatePortConfig(uint16_t divider, uint8_t lcr) {
    abortPacket();

    // Check whether port settings match mouse protocol,
    // to prevent false device detections by guest software

    mouse_port_valid = true;

    const uint8_t bytelen   = (lcr & 0x3) + 5;
    const bool    onestop   = !(lcr & 0x4);
    const uint8_t parity_id = (lcr & 0x38) >> 3;

    if (divider!= 96 || !onestop || // for mouse we need 1200 bauds and 1 stop bit
        parity_id == 1 || parity_id == 3 || parity_id == 5 || parity_id == 7) // parity has to be 'N'
        mouse_port_valid = false;

    if (mouse_port_valid && config_auto) { // auto select mouse type to emulate
        if (bytelen == 7) {
            applyType(config_type);
        } else if (bytelen == 8) {
            applyType(MouseType::MOUSE_SYSTEMS);           
        } else
            mouse_port_valid = false;
    }
    else if (mouse_bytelen != bytelen)     // byte length has to match between port and protocol
        mouse_port_valid = false;
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
        mouseReset();
    }

    setRTS(rts);
    setDTR(dtr);
}

void CSerialMouse::setRTS(bool val) {
    if (val && !getRTS() && getDTR()) {
        mouseReset();
    }

    setCTS(val);
}
void CSerialMouse::setDTR(bool val) {
    if (val && !getDTR() && getRTS()) {
        mouseReset();
    }

    setDSR(val);
    setRI(val);
    setCD(val);
}
