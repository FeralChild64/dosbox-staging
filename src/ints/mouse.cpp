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

#include <algorithm>

#include "bitops.h"
#include "callback.h"
#include "checks.h"
#include "cpu.h"
#include "keyboard.h"
#include "mem.h"
#include "pic.h"
#include "regs.h"

using namespace bit::literals;

CHECK_NARROWING();

// XXX TODO
// XXX MOUSEDOSDRV_HasCallback() - to be removed; there shouldn't be a call, but notification to mouse.cpp
// XXX MOUSEBIOS_HasCallback() - to be removed too,
// XXX use bit_view for mouse buttons whenever possible
// XXX fix IntelliMouse Explorer

MouseInfoConfig mouse_config;
MouseInfoVideo  mouse_video;

enum class EventType:uint8_t { // compatible with DOS driver mask in driver function 0x0c
    NotDosEvent    = 0,
    MouseHasMoved  = 1 << 0,
    PressedLeft    = 1 << 1,
    ReleasedLeft   = 1 << 2,
    PressedRight   = 1 << 3,
    ReleasedRight  = 1 << 4,
    PressedMiddle  = 1 << 5,
    ReleasedMiddle = 1 << 6,
    WheelHasMoved  = 1 << 7,
};

typedef struct MouseEvent {
    bool req_ps2 = false; // true = PS/2 mouse emulation needs an IRQ12 / INT74 event
    bool req_dos = false; // true = virtual DOS mouse driver needs an event

    uint8_t dos_type    = 0;
    uint8_t dos_buttons = 0; 

    MouseEvent() {}
    MouseEvent(EventType dos_type) : dos_type(static_cast<uint8_t>(dos_type)) {}

} MouseEvent;

static constexpr uint8_t QUEUE_SIZE = 32; // if over 255, increase 'queue_used' size

static MouseEvent queue[QUEUE_SIZE] = {};
static uint8_t    queue_used        = 0;
static bool       timer_in_progress = false;

static uintptr_t  int74_ret_callback = 0;

static uint8_t    buttons_12  = 0; // state of buttons 1 (left), 2 (right), as visible on host side
static uint8_t    buttons_345 = 0; // state of mouse buttons 3 (middle), 4, and 5 as visible on host side

// ***************************************************************************
// Queue / interrupt 74 implementation
// ***************************************************************************

static void EventHandler(uint32_t); // forward devlaration

static float GetEventDelay()
{
    if (MOUSEDOSDRV_HasCallback())
        return 5.0f; // 200 Hz sampling rate

    return MOUSEPS2AUX_GetDelay();
}

static void SendPacket()
{
    timer_in_progress = true;
    PIC_AddEvent(EventHandler, GetEventDelay());

    // Filter out unneeded DOS driver events
    auto &event = queue[queue_used - 1];
    event.req_dos &= MOUSEDOSDRV_HasCallback();

    // Send mouse event either via PS/2 bus or activate INT74/IRQ12 directly
    if (event.req_ps2) {
        MOUSEPS2AUX_UpdatePacket();
        PIC_ActivateIRQ(12); // temporary, until proper PS/2 hardware interface is implemented
    } else if (event.req_dos)
        PIC_ActivateIRQ(12);
}

static void EventHandler(uint32_t /*val*/)
{
    timer_in_progress = false;
    if (queue_used) SendPacket();
}

static void AddEvent(MouseEvent &event)
{
    // Filter out unneeded DOS driver events
    event.req_dos &= MOUSEDOSDRV_HasCallback();
    // PS/2 events are relevant even without BIOS callback,
    // they might be needed by register-level mouse access
    if (!event.req_ps2 && !event.req_dos)
        return; // Skip - no driver actually needs this event

    if (queue_used < QUEUE_SIZE) {
        // Events are handled starting from highest index
        if (queue_used) {
            // PS/2 notifications should be executed ASAP,
            // so move 'req_ps2' flag to the first event
            if (event.req_ps2) {
                queue[queue_used - 1].req_ps2 = true;
                if (!event.req_dos)
                    return; // Skip - no driver needs what is left
                event.req_ps2 = false;
            }
            // Skip redundant events
            if (event.dos_type == static_cast<uint8_t>(EventType::MouseHasMoved) ||
                event.dos_type == static_cast<uint8_t>(EventType::WheelHasMoved)) {
                return;
            }
            // This is a button event; put it at the back
            // to prevent doubleclicks while moving/scrolling
            for (auto i = queue_used ; i ; i--)
                queue[i] = queue[i - 1];
        }
        queue[0] = event;
        queue[0].dos_buttons = static_cast<uint8_t>(buttons_12 + (buttons_345 ? 4 : 0));
        queue_used++;
    }

    if (!timer_in_progress) SendPacket();
}

static EventType SelectEventPressed(const uint8_t idx, const bool changed_12S)
{
    switch (idx) {
    case 0:  return EventType::PressedLeft;
    case 1:  return EventType::PressedRight;
    case 2:  return EventType::PressedMiddle;
    case 3:
    case 4:  return changed_12S ? EventType::PressedMiddle : EventType::NotDosEvent;
    default: return EventType::NotDosEvent;
    }
}

static EventType SelectEventReleased(const uint8_t idx, const bool changed_12S)
{
    switch (idx) {
    case 0:  return EventType::ReleasedLeft;
    case 1:  return EventType::ReleasedRight;
    case 2:  return EventType::ReleasedMiddle;
    case 3:
    case 4:  return changed_12S ? EventType::ReleasedMiddle : EventType::NotDosEvent;
    default: return EventType::NotDosEvent;
    }
}

static uintptr_t INT74_Exit()
{
    SegSet16(cs, RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
    reg_ip = RealOff(CALLBACK_RealPointer(int74_ret_callback));

    return CBRET_NONE;  
}

static uintptr_t INT74_Handler()
{
    // If DOS mouse handler is busy - try the next time
    if (MOUSEDOSDRV_CallbackInProgress())
        return INT74_Exit();

    if (queue_used) {
        const auto &event = queue[--queue_used];

        // INT 33h emulation: HERE within the IRQ 12 handler is the appropriate place to
        // redraw the cursor. OSes like Windows 3.1 expect real-mode code to do it in
        // response to IRQ 12, not "out of the blue" from the SDL event handler like
        // the original DOSBox code did it. Doing this allows the INT 33h emulation
        // to draw the cursor while not causing Windows 3.1 to crash or behave
        // erratically.
        MOUSEDOSDRV_DrawCursor();

        if (MOUSEDOSDRV_HasCallback()) {
            // To be handled by DOS mouse driver
            if (!MOUSEDOSDRV_HasCallback(event.dos_type) || !event.req_dos)
                return INT74_Exit();
            CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
            CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)) + 7);
            return MOUSEDOSDRV_DoCallback(event.dos_type, event.dos_buttons);
        }
    }

    if (MOUSEBIOS_HasCallback()) {
        // To be handled by PS/2 BIOS
        CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
        CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)));
        return MOUSEBIOS_DoCallback();
    }

    // No events
    return INT74_Exit();
}

uintptr_t INT74_Ret_Handler()
{
    if (queue_used && !timer_in_progress) {
        timer_in_progress = true;
        PIC_AddEvent(EventHandler, GetEventDelay());
    }
    return CBRET_NONE;
}

void MOUSE_ClearQueue()
{
    queue_used        = 0;
    timer_in_progress = false;

    PIC_RemoveEvents(EventHandler);
}

// ***************************************************************************
// External notifications
// ***************************************************************************

void MOUSE_SetSensitivity(const int32_t sensitivity_x, const int32_t sensitivity_y)
{
    auto adapt = [](const int32_t sensitivity)
    {
        constexpr float MIN = 0.01f;
        constexpr float MAX = 100.0f;

        const float tmp = std::clamp(static_cast<float>(sensitivity) / 100.0f, -MAX, MAX);

        if (tmp >= 0)
            return std::max(tmp, MIN);
        else
            return std::min(tmp, -MIN);
    };

    mouse_config.sensitivity_x = adapt(sensitivity_x);
    mouse_config.sensitivity_y = adapt(sensitivity_y);
}

void MOUSE_NewScreenParams(const uint16_t clip_x, const uint16_t clip_y, const uint16_t res_x, const uint16_t res_y,
                           const bool fullscreen, const uint16_t x_abs, const uint16_t y_abs)
{

    mouse_video.clip_x = clip_x;
    mouse_video.clip_y = clip_y;

    // Protection against strange window sizes,
    // to prevent division by 0 in some places
    mouse_video.res_x = std::max(res_x, static_cast<uint16_t>(2));
    mouse_video.res_y = std::max(res_y, static_cast<uint16_t>(2));

    mouse_video.fullscreen = fullscreen;

    MOUSEVMWARE_NewScreenParams(x_abs, y_abs);
}

void MOUSE_EventMoved(const int16_t x_rel, const int16_t y_rel, const uint16_t x_abs, const uint16_t y_abs, const bool is_captured) {

    MouseEvent event(EventType::MouseHasMoved);

    event.req_ps2 = MOUSEPS2AUX_NotifyMoved(x_rel, y_rel);
    event.req_ps2 = MOUSEVMWARE_NotifyMoved(x_abs, y_abs) || event.req_ps2;
    event.req_dos = MOUSEDOSDRV_NotifyMoved(x_rel, y_rel, x_abs, y_abs, is_captured);
    MOUSESERIAL_NotifyMoved(x_rel, y_rel);

    AddEvent(event);
}

void MOUSE_NotifyMovedFake() {

    MouseEvent event(EventType::MouseHasMoved);
    event.req_ps2 = true;

    AddEvent(event);
}

void MOUSE_EventPressed(uint8_t idx)
{
    const uint8_t buttons_12S_old = static_cast<uint8_t>(buttons_12 + (buttons_345 ? 4 : 0));

    switch (idx) {
    case 0: // left button
        if (bit::is(buttons_12, b0)) return;
        bit::set(buttons_12, b0);
        break;
    case 1: // right button
        if (bit::is(buttons_12, b1)) return;
        bit::set(buttons_12, b1);
        break;
    case 2: // middle button
        if (bit::is(buttons_345, b2)) return;
        bit::set(buttons_345, b2);
        break;
    case 3: // extra button #1
        if (bit::is(buttons_345, b3)) return;
        bit::set(buttons_345, b3);
        break;
    case 4: // extra button #2
        if (bit::is(buttons_345, b4)) return;
        bit::set(buttons_345, b4);
        break;
    default: // button not supported
        return;
    }

    const uint8_t buttons_12S = static_cast<uint8_t>(buttons_12 + (buttons_345 ? 4 : 0));
    const bool    changed_12S = (buttons_12S_old != buttons_12S);
    const uint8_t idx_12S     = idx < 2 ? idx : 2;

    MouseEvent event(SelectEventPressed(idx, changed_12S));

    event.req_ps2 = MOUSEPS2AUX_NotifyPressedReleased(buttons_12S, buttons_12 | buttons_345);
    if (changed_12S) {
        event.req_ps2 = MOUSEVMWARE_NotifyPressedReleased(buttons_12S) || event.req_ps2;
        event.req_dos = MOUSEDOSDRV_NotifyPressed(buttons_12S, idx_12S);
        MOUSESERIAL_NotifyPressed(buttons_12S, idx_12S);
    }

    AddEvent(event);
}

void MOUSE_EventReleased(uint8_t idx) {
    const uint8_t buttons_12S_old = static_cast<uint8_t>(buttons_12 + (buttons_345 ? 4 : 0));

    switch (idx) {
    case 0: // left button
        if (bit::cleared(buttons_12, b0)) return;
        bit::clear(buttons_12, b0);
        break;
    case 1: // right button
        if (bit::cleared(buttons_12, b1)) return;
        bit::clear(buttons_12, b1);
        break;
    case 2: // middle button
        if (bit::cleared(buttons_345, b2)) return;
        bit::clear(buttons_345, b2);
        break;
    case 3: // extra button #1
        if (bit::cleared(buttons_345, b3)) return;
        bit::clear(buttons_345, b3);
        break;
    case 4: // extra button #2
        if (bit::cleared(buttons_345, b4)) return;
        bit::clear(buttons_345, b4);
        break;
    default: // button not supported
        return;
    }

    const uint8_t buttons_12S = static_cast<uint8_t>(buttons_12 + (buttons_345 ? 4 : 0));
    const bool    changed_12S = (buttons_12S_old != buttons_12S);
    const uint8_t idx_12S     = idx < 2 ? idx : 2;

    MouseEvent event(SelectEventReleased(idx, changed_12S));

    event.req_ps2 = MOUSEPS2AUX_NotifyPressedReleased(buttons_12S, buttons_12 | buttons_345);
    if (changed_12S) {
        event.req_ps2 = MOUSEVMWARE_NotifyPressedReleased(buttons_12S) || event.req_ps2;
        event.req_dos = MOUSEDOSDRV_NotifyReleased(buttons_12S, idx_12S);
        MOUSESERIAL_NotifyReleased(buttons_12S, idx_12S);
    }

    AddEvent(event);
}

void MOUSE_EventWheel(const int16_t w_rel)
{
    if (w_rel == 0) return;

    MouseEvent event(EventType::WheelHasMoved);

    event.req_ps2 = MOUSEPS2AUX_NotifyWheel(w_rel);
    event.req_ps2 = MOUSEVMWARE_NotifyWheel(w_rel) || event.req_ps2;
    event.req_dos = MOUSEDOSDRV_NotifyWheel(w_rel);
    MOUSESERIAL_NotifyWheel(w_rel);

    AddEvent(event);
}

// ***************************************************************************
// Initialization
// ***************************************************************************

void MOUSE_Init(Section* /*sec*/)
{
    // Callback for ps2 irq
    auto call_int74 = CALLBACK_Allocate();
    CALLBACK_Setup(call_int74, &INT74_Handler, CB_IRQ12, "int 74");
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

    int74_ret_callback = CALLBACK_Allocate();
    CALLBACK_Setup(int74_ret_callback, &INT74_Ret_Handler, CB_IRQ12_RET, "int 74 ret");
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

    // (MOUSE_IRQ > 7) ? (0x70 + MOUSE_IRQ - 8) : (0x8 + MOUSE_IRQ);
    RealSetVec(0x74, CALLBACK_RealPointer(call_int74));

    MOUSEPS2AUX_Init();
    MOUSEVMWARE_Init();
    MOUSEDOSDRV_Init();
}
