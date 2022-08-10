/*
 *  Copyright (C) 2022       The DOSBox Staging Team
 *  Copyright (C) 2004-2021  The Bochs Project
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

// Adapted from Bochs emulator code:
// - https://github.com/stlintel/Bochs/blob/master/bochs/iodev/busmouse.cc
// The original code was licensed under GNU Lesser General Public License
// version 2 or above - which allows to relicense the code under
// GNU General Public License version 2 or above (used by DOSBox Staging).

// Initial/additional code by Ben Lunt 'fys at fysnet dot net'
//   http://www.fysnet.net

#include "mouse.h"
#include "mouse_config.h"
#include "mouse_internals.h"
#include "mouse_interfaces.h"

#include <algorithm>
#include <cmath>

#include "bitops.h"
#include "checks.h"
#include "pic.h"

CHECK_NARROWING();

// - XXX check ftp://x2ftp.oulu.fi/pub/msdos/programming/docs/gmouse.doc

// ----


// XXX use mouse_config.bus_irq


enum Control : uint8_t // XXX turn into enum class
{
    ReadButtons = 0x00,
    ReadX       = 0x01,
    ReadY       = 0x02,
    Command     = 0x07,
    RaiseIrq    = 0x16,
    Reset       = 0x80,
};

static struct {
    uint8_t control   = 0;
    uint8_t config    = 0;
    bool interrupts   = false;
    uint8_t signature = 0;
    bool    toggle_counter = false;

} state;



// XXX cleanup this!

#define IRQ_MASK ((1<<5) >> mouse_config.bus_irq)

#define INP_HOLD_COUNTER      (1 << 5)
#define INP_ENABLE_IRQ        (1 << 0)

#define HOLD_COUNTER  (1 << 7)
#define READ_X        (0 << 6)
#define READ_Y        (1 << 6)
#define READ_LOW      (0 << 5)
#define READ_HIGH     (1 << 5)
#define DISABLE_IRQ   (1 << 4)

#define READ_X_LOW    (READ_X | READ_LOW)
#define READ_X_HIGH   (READ_X | READ_HIGH)
#define READ_Y_LOW    (READ_Y | READ_LOW)
#define READ_Y_HIGH   (READ_Y | READ_HIGH)

// XXX put helper routines

uint8_t get_value_byte(const io_val_t value)
{
    return static_cast<uint8_t>(value & 0xff);
}

// ***************************************************************************
// Bus/InPort mouse - data register
// ***************************************************************************

static void inp_write_data(io_port_t, io_val_t value, io_width_t)
{
    const auto value_byte = get_value_byte(value);

/* XXX
    PIC_DeActivateIRQ(mouse_config.bus_irq);
    if (value_byte == INP_CTRL_RAISE_IRQ)
        PIC_ActivateIRQ(mouse_config.bus_irq);
    else if (state.cmmand == NP_CTRL_COMMAND) {
        state.control = value_byte;
        state.interrupts = (value_byte & INP_ENABLE_IRQ);
    } */
}

static uint32_t bus_read_data(const io_port_t, const io_width_t)
{
    // XXX
/*
    switch (state.control & 0x60) {
    case READ_X_LOW:
        return BX_BUSM_THIS current_x & 0x0F;
        break;
    case READ_X_HIGH:
        return (BX_BUSM_THIS current_x >> 4) & 0x0F;
        break;
    case READ_Y_LOW:
        return BX_BUSM_THIS current_y & 0x0F;
        break;
    case READ_Y_HIGH:
        return ((BX_BUSM_THIS current_b ^ 7) << 5) | ((BX_BUSM_THIS current_y >> 4) & 0x0F);
    default:
        break;
    }
*/

    return 0;
}

static uint32_t inp_read_data(const io_port_t, const io_width_t)
{
    // XXX
/*
    switch (state.cmmand) {
    case INP_CTRL_READ_BUTTONS:
        // Newer Logitech mouse drivers look for bit 6 set
        return (BX_BUSM_THIS current_b) | 0x40;
    case INP_CTRL_READ_X:
        return BX_BUSM_THIS current_x;
    case INP_CTRL_READ_Y:
        return BX_BUSM_THIS current_y;
    case INP_CTRL_COMMAND:
        return state.control;
    default:
        break;
    }
    break;
*/

    return 0;
}

// ***************************************************************************
// Bus/InPort mouse - control register
// ***************************************************************************

static void bus_write_control(io_port_t, io_val_t value, io_width_t)
{
    const auto value_byte = get_value_byte(value);

    state.control = value_byte | 0x0f;
    state.interrupts = !(value_byte & DISABLE_IRQ);
    PIC_DeActivateIRQ(mouse_config.bus_irq);
}

static void inp_write_control(io_port_t, io_val_t value, io_width_t)
{
    const auto value_byte = get_value_byte(value);

/* XXX
    switch (value_byte) {
    case INP_CTRL_RESET:
        state.control = 0;
        state.cmmand = 0;
        break;
    case INP_CTRL_COMMAND:
    case INP_CTRL_READ_BUTTONS:
    case INP_CTRL_READ_X:
    case INP_CTRL_READ_Y:
        state.cmmand = value_byte;
        break;
    case 0x87:  // TODO: ???????
        state.control = 0;
        state.cmmand  = 0x07;
        break;
    default:
        break;
    } */
}

static uint32_t bus_read_control(const io_port_t, const io_width_t)
{
    // XXX
/*
    value = state.control;
    // this is to allow the driver to see which IRQ the card has "jumpered"
    // only happens if IRQ's are enabled
    state.control |= 0x0F;
    if ((BX_BUSM_THIS toggle_counter > 0x3FF) && state.interrupts)
        // newer hardware only changes the bit when interrupts are on
        // older hardware changes the bit no matter if interrupts are on or not
        state.control &= ~IRQ_MASK;
    BX_BUSM_THIS toggle_counter = (BX_BUSM_THIS toggle_counter + 1) & 0x7FF;

    return value;
*/


    return 0;
}

static uint32_t inp_read_control(const io_port_t, const io_width_t)
{
    return state.control;
}

// ***************************************************************************
// Bus/InPort mouse - config register
// ***************************************************************************

static void bus_write_config(io_port_t, io_val_t value, io_width_t)
{
    state.config = get_value_byte(value);
}

static uint32_t bus_read_config(const io_port_t, const io_width_t)
{
    return state.config;
}

// ***************************************************************************
// Bus/InPort mouse - signature register
// ***************************************************************************

static void bus_write_signature(io_port_t, io_val_t value, io_width_t)
{
    state.signature = get_value_byte(value);
}

static uint32_t bus_read_signature(const io_port_t, const io_width_t)
{
    return state.signature;
}

static uint32_t inp_read_signature(const io_port_t, const io_width_t)
{
    if (state.toggle_counter) {
        state.toggle_counter = false;
        return 0x12; // Manufacturer ID?
    } else {
        state.toggle_counter = true;
        return 0xde;
    }
}

// ***************************************************************************
// Bus/InPort mouse interface implementation
// ***************************************************************************

static uint32_t dummy_read(const io_port_t, const io_width_t)
{
    return 0; // not supported
}

static void dummy_write(io_port_t, io_val_t value, io_width_t)
{
    return; // not supported
}

bool MOUSEBUS_NotifyMoved(const float x_rel, const float y_rel)
{
    // XXX

    return false;
}

bool MOUSEBUS_NotifyButton(const MouseButtons12S buttons_12S)
{
    // XXX

    return false;
}

void MOUSEBUS_Update()
{
    // XXX
}

void MOUSEBUS_Init()
{
    LOG_ERR("XXX - NOT YET READY!!!");

    auto register_handlers = [](const uint8_t port,
                                const io_read_f read_handler,
                                const io_write_f write_handler) {
        IO_RegisterReadHandler(mouse_config.bus_base + port,
                               read_handler,
                               io_width_t::byte);
        IO_RegisterWriteHandler(mouse_config.bus_base + port,
                                write_handler,
                                io_width_t::byte);
    };

    // XXX notify about mouse rate

    switch (mouse_config.model_bus) {
    case MouseModelBus::Bus:
        LOG_MSG("MOUSE (BUS): Logitech / Microsoft, Bus Mouse on port 0x%03x", mouse_config.bus_base);
        register_handlers(0, bus_read_data,      dummy_write);
        register_handlers(1, bus_read_signature, bus_write_signature);
        register_handlers(2, bus_read_control,   bus_write_control);
        register_handlers(3, bus_read_config,    bus_write_config);
        state.control = 0x1f;
        state.config  = 0x0e;
        break;
    case MouseModelBus::InPort:
        LOG_MSG("MOUSE (BUS): Microsoft, InPort Mouse on port 0x%03x", mouse_config.bus_base);
        register_handlers(0, inp_read_control,   inp_write_control);
        register_handlers(1, inp_read_data,      inp_write_data);
        register_handlers(2, inp_read_signature, dummy_write);
        register_handlers(3, dummy_read,         dummy_write);
        break;
    default:
        LOG_ERR("MOUSE (BUS): Invalid type");
        assert(false); // unimplemented
        break;
    }
}
