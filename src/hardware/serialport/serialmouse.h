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

#ifndef DOSBOX_SERIALMOUSE_H
#define DOSBOX_SERIALMOUSE_H

#include "serialport.h"


class CSerialMouse : public CSerial {
public:
    CSerialMouse(Bitu id, CommandLine* cmd);
    virtual ~CSerialMouse();

    void onMouseEvent(Bit16s delta_x, Bit16s delta_y, Bit16s delta_w, Bit8u buttons);

    void setRTSDTR(bool rts, bool dtr);
    void setRTS(bool val);
    void setDTR(bool val);

    void updatePortConfig(uint16_t divider, uint8_t lcr);
    void updateMSR();
    void transmitByte(Bit8u val, bool first);
    void setBreak(bool value);
    void handleUpperEvent(Bit16u type);

private:

    void onMouseReset();
    void startPacket();

    bool   send_ack;
    Bit8u  packet[3] = {};
    Bit8u  packet_xmit;
    bool   xmit_another_packet;
    Bit8u  mouse_buttons;  // bit 0 = left, bit 1 = right, bit 2 = middle
    Bit32s mouse_delta_x, mouse_delta_y, mouse_delta_w;
};

#endif // DOSBOX_SERIALMOUSE_H
