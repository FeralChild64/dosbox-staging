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

#include "callback.h"
#include "cpu.h"
#include "keyboard.h"
#include "mem.h"
#include "pic.h"
#include "regs.h"

enum DOS_EV:Bit16u { // compatible with DOS driver mask in driver function 0x0c
    NOT_DOS_EVENT     =   0x00,
    MOUSE_MOVED       =   0x01,
    PRESSED_LEFT      =   0x02,
    RELEASED_LEFT     =   0x04,
    PRESSED_RIGHT     =   0x08,
    RELEASED_RIGHT    =   0x10,
    PRESSED_MIDDLE    =   0x20,
    RELEASED_MIDDLE   =   0x40,
    WHEEL_MOVED       =   0x80,
};

static const Bit8u QUEUE_SIZE  = 32; // if over 255, increase 'events' size

static const Bit8u KEY_MASKS[] = { 0x01, 0x02, 0x04, 0x08, 0x10 }; // XXX utilize bit manipulation from DOSBox Staging instead

static Bit8u    buttons_12;                   // state of buttons 1 (left), 2 (right), as visible on host side
static Bit8u    buttons_345;                  // state of mouse buttons 3 (middle), 4, and 5 as visible on host side

static struct { Bit8u type; Bit8u buttons; } event_queue[QUEUE_SIZE]; // XXX rename to reflect that this is only DOS related
static Bit8u    events;
static bool     timer_in_progress;

static Bitu     call_int74, int74_ret_callback;
static bool     int74_used;                   // true = our virtual INT74 callback is actually used, not overridden
static Bit8u    int74_needed_countdown;       // counter to detect above value

MouseInfoConfig mouse_config;
MouseInfoVideo  mouse_video;

// ***************************************************************************
// Queue / interrupt 74 implementation
// ***************************************************************************

static void EventHandler(uint32_t); // forward devlaration

static inline float GetEventDelay() {
    if (int74_used && MouseDOS_HasCallback(0xff))
        return 5.0f; // 200 Hz sampling rate

    return MousePS2_GetDelay();
}

static inline void SendPacket() {
    if (int74_needed_countdown > 0)
        int74_needed_countdown--;
    else
        int74_used = false;

    timer_in_progress = true;
    PIC_AddEvent(EventHandler, GetEventDelay());
    MousePS2_SendPacket(); // this will trigger IRQ 12 / INT 74
}

static void EventHandler(uint32_t /*val*/)
{
    timer_in_progress = false;
    if (events) SendPacket();
}

static void AddEvent(Bit8u type) {
    if (events < QUEUE_SIZE) {
        if (events > 0) {
            // Skip redundant events
            if (type == DOS_EV::MOUSE_MOVED || type == DOS_EV::WHEEL_MOVED) return;
            // Always put the newest element in the front as that the events are 
            // handled backwards (prevents doubleclicks while moving)
            for(Bitu i = events ; i ; i--)
                event_queue[i] = event_queue[i - 1];
        }
        event_queue[0].type    = type;
        event_queue[0].buttons = buttons_12 + (buttons_345 ? 4 : 0);
        events++;
    }

    if (!timer_in_progress) SendPacket();
}

static inline DOS_EV SelectEventPressed(Bit8u idx, bool changed_12S) {
    switch (idx) {
    case 0:  return DOS_EV::PRESSED_LEFT;
    case 1:  return DOS_EV::PRESSED_RIGHT;
    case 2:  return DOS_EV::PRESSED_MIDDLE;
    case 3:  return changed_12S ? DOS_EV::PRESSED_MIDDLE : DOS_EV::NOT_DOS_EVENT;
    case 4:  return changed_12S ? DOS_EV::PRESSED_MIDDLE : DOS_EV::NOT_DOS_EVENT;
    default: return DOS_EV::NOT_DOS_EVENT;
    }
}

static inline DOS_EV SelectEventReleased(Bit8u idx, bool changed_12S) {
    switch (idx) {
    case 0:  return DOS_EV::RELEASED_LEFT;
    case 1:  return DOS_EV::RELEASED_RIGHT;
    case 2:  return DOS_EV::RELEASED_MIDDLE;
    case 3:  return changed_12S ? DOS_EV::RELEASED_MIDDLE : DOS_EV::NOT_DOS_EVENT;
    case 4:  return changed_12S ? DOS_EV::RELEASED_MIDDLE : DOS_EV::NOT_DOS_EVENT;
    default: return DOS_EV::NOT_DOS_EVENT;
    }
}

static Bitu INT74_Handler() {
    int74_used = true;
    int74_needed_countdown = 5;

    KEYBOARD_ClrMsgAUX(); // XXX it should probably only clear last 3 or 4 bytes, depending on last packet size

    // XXX fix spamming INT74's under Windows 3.1; something to do with protected mode?
    // LOG_WARNING("XXX - SPAM SPAM SPAM");

    if (events > 0 && !MouseDOS_CallbackInProgress()) {
        events--;

        // INT 33h emulation: HERE within the IRQ 12 handler is the appropriate place to
        // redraw the cursor. OSes like Windows 3.1 expect real-mode code to do it in
        // response to IRQ 12, not "out of the blue" from the SDL event handler like
        // the original DOSBox code did it. Doing this allows the INT 33h emulation
        // to draw the cursor while not causing Windows 3.1 to crash or behave
        // erratically.
        MouseDOS_DrawCursor();

        if (MouseDOS_HasCallback(event_queue[events].type)) {

            CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
            CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)) + 7);
            return MouseDOS_DoCallback(event_queue[events].type, event_queue[events].buttons);
        }
        else if (MouseBIOS_HasCallback()) {

            CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
            CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)));
            return MouseBIOS_DoCallback();
        }
    }

    // No events or handler busy
    SegSet16(cs, RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
    reg_ip = RealOff(CALLBACK_RealPointer(int74_ret_callback));

    return CBRET_NONE;
}

Bitu INT74_Ret_Handler() {
    if (events) {
        if (!timer_in_progress) {
            timer_in_progress = true;
            PIC_AddEvent(EventHandler, GetEventDelay());
        }
    }
    return CBRET_NONE;
}

void Mouse_ClearQueue() {
    events            = 0;
    timer_in_progress = false;

    PIC_RemoveEvents(EventHandler);
}

// ***************************************************************************
// External notifications
// ***************************************************************************

void Mouse_SetSensitivity(Bit32s sensitivity_x, Bit32s sensitivity_y) {
    static constexpr float MIN = 0.01f;
    static constexpr float MAX = 100.0f;

    mouse_config.sensitivity_x = std::clamp(sensitivity_x/100.0f, -MAX, MAX);
    if (!std::signbit(mouse_config.sensitivity_x))
        mouse_config.sensitivity_x = std::max(mouse_config.sensitivity_x, MIN);
    else
        mouse_config.sensitivity_x = std::min(mouse_config.sensitivity_x, -MIN);

    mouse_config.sensitivity_y = std::clamp(sensitivity_y/100.0f, -MAX, MAX);
    if (!std::signbit(mouse_config.sensitivity_y))
        mouse_config.sensitivity_y = std::max(mouse_config.sensitivity_y, MIN);
    else
        mouse_config.sensitivity_y = std::min(mouse_config.sensitivity_y, -MIN);
}

void Mouse_NewScreenParams(Bit16u clip_x, Bit16u clip_y,
                           Bit16u res_x,  Bit16u res_y,
                           bool fullscreen,
                           Bit32s x_abs, Bit32s y_abs) {

    mouse_video.clip_x     = clip_x;
    mouse_video.clip_y     = clip_y;
    mouse_video.res_x      = res_x;
    mouse_video.res_y      = res_y;
    mouse_video.fullscreen = fullscreen;

    MouseVMW_NewScreenParams(x_abs, y_abs);
}

void Mouse_EventMoved(Bit32s x_rel, Bit32s y_rel, Bit32s x_abs, Bit32s y_abs, bool is_captured) {
    if (x_rel != 0 || y_rel != 0) {
        MousePS2_NotifyMoved(x_rel, y_rel);
        MouseVMW_NotifyMoved(x_abs, y_abs);
        MouseDOS_NotifyMoved(x_rel, y_rel, is_captured);
        MouseSER_NotifyMoved(x_rel, y_rel);

        AddEvent(DOS_EV::MOUSE_MOVED);
    }
}

void MousePS2_NotifyMovedDummy() {
    // XXX we need a better implementation here - something might still be waiting in event queue
    MousePS2_SendPacket(true);
}

void Mouse_EventPressed(Bit8u idx) {
    Bit8u buttons_12S_old = buttons_12 + (buttons_345 ? 4 : 0);

    if (idx < 2) {
        // left/right button
        if (buttons_12 & KEY_MASKS[idx]) return;
        buttons_12 |= KEY_MASKS[idx];
    } else if (idx < 5) {
        // middle/extra button
        if (buttons_345 & KEY_MASKS[idx]) return;
        buttons_345 |= KEY_MASKS[idx];
    } else
        return; // button not supported

    Bit8u buttons_12S = buttons_12 + (buttons_345 ? 4 : 0);
    bool  changed_12S = (buttons_12S_old != buttons_12S);
    Bit8u idx_12S     = idx < 2 ? idx : 2;

    MousePS2_NotifyPressedReleased(buttons_12S, buttons_12 | buttons_345);
    if (changed_12S) {
        MouseDOS_NotifyPressed(buttons_12S, idx_12S);
        MouseVMW_NotifyPressedReleased(buttons_12S);
        MouseSER_NotifyPressed(buttons_12S, idx_12S);
    }

    AddEvent(SelectEventPressed(idx, changed_12S));
}

void Mouse_EventReleased(Bit8u idx) {
    Bit8u buttons_12S_old = buttons_12 + (buttons_345 ? 4 : 0);

    if (idx < 2) {
        // left/right button
        if (!(buttons_12 & KEY_MASKS[idx])) return;
        buttons_12 &= ~KEY_MASKS[idx];
    } else if (idx < 5) {
        // middle/extra button
        if (!(buttons_345 & KEY_MASKS[idx])) return;
        buttons_345 &= ~KEY_MASKS[idx];
    } else
        return; // button not supported

    Bit8u buttons_12S = buttons_12 + (buttons_345 ? 4 : 0);
    bool  changed_12S = (buttons_12S_old != buttons_12S);
    Bit8u idx_12S     = idx < 2 ? idx : 2;

    MousePS2_NotifyPressedReleased(buttons_12S, buttons_12 | buttons_345);
    if (changed_12S) {
        MouseDOS_NotifyReleased(buttons_12S, idx_12S);
        MouseVMW_NotifyPressedReleased(buttons_12S);
        MouseSER_NotifyReleased(buttons_12S, idx_12S);
    }

    AddEvent(SelectEventReleased(idx, changed_12S));
}

void Mouse_EventWheel(Bit32s w_rel) {
    if (w_rel != 0) {
        MousePS2_NotifyWheel(w_rel);
        MouseVMW_NotifyWheel(w_rel);
        MouseDOS_NotifyWheel(w_rel);
        MouseSER_NotifyWheel(w_rel);

        AddEvent(DOS_EV::WHEEL_MOVED);
    }
}

// ***************************************************************************
// Initialization
// ***************************************************************************

void MOUSE_Init(Section* /*sec*/) {

    // Callback for ps2 irq
    call_int74 = CALLBACK_Allocate();
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

    MousePS2_Init();
    MouseVMW_Init();
    MouseDOS_Init();
}
