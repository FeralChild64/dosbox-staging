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
#include "mouse_core.h"

#include <algorithm>

#include "callback.h"
#include "checks.h"
#include "cpu.h"
#include "keyboard.h"
#include "mem.h"
#include "pic.h"
#include "regs.h"
#include "video.h"

CHECK_NARROWING();

// TODO before next PR:
//
// XXX review 0.499f / 0.6f and std::round usage, should be used everywhere, 0.499f / 0.6f should be a define
// XXX review once again all std::clamp, std::min, std::max with floating point values (fmax, fmin should be more simple)
// XXX fix various remarks in mouse_dos_driver.cpp
// XXX review BIOS/DOS mouse with ralph brown interrupt list (part N for dos driver)
// XXX finish the new queue
// XXX fix support for windows driver from https://git.javispedro.com/cgit/vbados.git
// XXX rename MOUSEVMWARE to MOUSEVMM

// XXX already finished, to be put in PR notes:
//
// User visible changes
// - CuteMouse 2.1 (not 2.0!), if started with /O command line option, is now able
//   to use wheel on emulated PS/2 mouse
// - software banging mouse BIOS directly (like Windows 3.x) can now show cursor behavior
//   much more similar original hardware, especially with raw input enabled
// - strange effects in windowed mode with seamless mouse control enabled with some software
//   (like DOOM or Windows 3.1) should be gone in 99% cases - just capture mouse manually
//   by pressing the scroll wheel (middle button) to use the mouse with them
 // Internal changes
// - large refactoring - BIOS+PS/2 and DOS driver mouse interfaces were put into separate files
// - implemented several cleanups suggested during 'part 1' pull requests, but were too risky
//   and complicated to apply on non-refactored code
// - Windows 3.x + DOS driver mouse stability fix ported from DOSBox X
// - it should be now much harder (if not impossible) to crash emulator using INT33 function 0x17
//   with malicious data
// - implemented more BIOS PS/2 mouse functions

MouseInfoActive mouse_active;
MouseInfoVideo  mouse_video;

bool mouse_seamless_driver = false;

static uintptr_t int74_ret_callback = 0;

static MouseButtons12  buttons_12   = 0; // host side state of buttons 1 (left), 2 (right)
static MouseButtons345 buttons_345  = 0; // host side state of buttons 3 (middle), 4, and 5

static float sensitivity_x = 0.3f; // sensitivity, might depend on the GUI/GFX
static float sensitivity_y = 0.3f; // for scaling all relative mouse movements

// ***************************************************************************
// Mouse button helper functions
// ***************************************************************************

static MouseButtonsAll GetButtonsJoined()
{
    MouseButtonsAll buttons_all;
    buttons_all.data = buttons_12.data | buttons_345.data;

    return buttons_all;
}

static MouseButtons12S GetButtonsSquished()
{
    MouseButtons12S buttons_12S;

    // Squish buttons 3/4/5 into single virtual middle button
    buttons_12S.data = buttons_12.data;
    if (buttons_345.data)
        buttons_12S.middle = 1;

    return buttons_12S;
}

// ***************************************************************************
// Mouse event queue implementation
// ***************************************************************************

typedef struct MouseEvent {

    bool req_ps2 = false; // if PS/2 mouse emulation needs an event
    bool req_vmm = false; // if virtual machine mouse needs an event
    bool req_dos = false; // if DOS mouse driver needs an event

    MouseEventId    event_id = MouseEventId::NotDosEvent; // XXX rename to id
    MouseButtons12S buttons_12S = 0;

    MouseEvent() {}
    MouseEvent(MouseEventId event_id) : event_id(event_id) {}

} MouseEvent;

class MouseQueue final {

public:

    MouseQueue();

    void AddEvent(MouseEvent &event);
    void FetchEvent(MouseEvent &event);
    void ClearDos();
    void StartTimer();
    void Tick();

private:

    MouseQueue(const MouseQueue&) = delete;
    MouseQueue &operator=(const MouseQueue&) = delete;

    bool IsEmpty() const;

    std::array<MouseEvent, 12> events = {}; // a modulo queue of events
    uint8_t idx_first    = 0;  // index of the first event
    uint8_t num_events   = 0;  // number of events in the queue

    MouseButtons12S last_buttons_12S = 0; // last buttons reported

    bool queue_overflowed  = false;
    bool timer_in_progress = false;

    // High prioroity events, handled before the queue

    bool event_ps2       = false;
    bool event_dos_moved = false;
    bool event_dos_wheel = false;

    bool prefer_ps2      = false; // true = next time prefer PS/2 events

} queue;

static void MouseQueueTick(uint32_t)
{
    queue.Tick();
}

MouseQueue::MouseQueue() { }

void MouseQueue::AddEvent(MouseEvent &event)
{
    // Since events are being fetched, clear the overflow flag
    if (mouse_active.dos && !mouse_active.dos_cb_running)
        queue_overflowed = false;

    if (event.req_ps2 || event.req_vmm) {
        // Events for PS/2 interfaces (or virtualizer compatible drivers)
        // do not carry any informations - they are only notifications
        // that new data is available for fetching
        event_ps2 = true;
    }

    // Prevent double-click while mouse is moving or wheel is moving
    if ((event_dos_moved && event.event_id == MouseEventId::MouseHasMoved) ||
        (event_dos_wheel && event.event_id == MouseEventId::WheelHasMoved))
        event.req_dos = false; // DOS queue already has such event

    // If queue got overflowed due to DOS not taking events,
    // don't accept any more button events, as it might lead to
    // strange effects in DOS application
    if (queue_overflowed &&
        event.event_id != MouseEventId::MouseHasMoved &&
        event.event_id != MouseEventId::WheelHasMoved) {
        event.req_dos = false;
        last_buttons_12S = event.buttons_12S;
    }

    if (event.req_dos) {
        if (event.event_id == MouseEventId::MouseHasMoved) {
            // Mouse has moved - put in priority place
            event_dos_moved = true;
        }
        else if (event.event_id == MouseEventId::WheelHasMoved) {
            // Wheel has moved - put in priority place
            event_dos_wheel = true;
        }
        else if (num_events >= events.size()) {
            // No space left, queue overflow. Clear it and don't
            // accept any more button events until application starts
            // to react again
            num_events = 0;
            queue_overflowed = true;
            last_buttons_12S = event.buttons_12S;
        }
        else {
            // Button press/release - put into the queue
            const auto idx = (idx_first + num_events) % events.size();
            num_events++;
            event.buttons_12S.data = GetButtonsSquished().data;
            events[idx] = event;
        }
    }

    // Filter out unnecessary events
    if (!event.req_dos && !event.req_ps2 && !event.req_vmm)
        return; //event not relevant for any mouse

    // If no timer in progress, launch the event immediately
    if (!timer_in_progress)
        Tick();
}

void MouseQueue::FetchEvent(MouseEvent &event)
{
    const bool event_dos = (num_events != 0) || event_dos_moved || event_dos_wheel;

    // We should prefer PS/2 events now (as the last was DOS one),
    // but we can't if there is no PS/2 event
    if (!event_ps2)
        prefer_ps2 = false;

    // First try DOS event (don't do this if handler is busy)
    if (!prefer_ps2 && event_dos && !mouse_active.dos_cb_running) {
        // Next time prefer PS/2 events
        prefer_ps2 = true;

        // Try priviledged DOS events (movement and wheel)
        if (event_dos_moved) {
            event.req_dos     = true;
            event.event_id    = MouseEventId::MouseHasMoved;
            event.buttons_12S = last_buttons_12S;
            event_dos_moved = false;
            return;
        } 
        if (event_dos_wheel) {
            event.req_dos     = true;
            event.event_id    = MouseEventId::WheelHasMoved;
            event.buttons_12S = last_buttons_12S;
            event_dos_wheel = false;
            return;
        }

        // If we are here, we have to provide button-related
        // event from the regular queue
        event = events[idx_first];
        last_buttons_12S = event.buttons_12S;
        idx_first = static_cast<uint8_t>((idx_first + 1) % events.size());
        num_events--;
        return;
    }

    // Now try PS/2 event
    if (event_ps2) {
        // Next time prefer DOS event
        prefer_ps2 = false;
        // PS/2 events are really dummy - merely a notification
        // that something has happened and driver has to react
        event.req_ps2 = true;   
        return;
    }

    // Nothing to provide, event will stay empty
}

bool MouseQueue::IsEmpty() const
{
    return !event_ps2 && !event_dos_moved && !event_dos_wheel &&
           (num_events == 0);
}

void MouseQueue::ClearDos()
{
    // Clear DOS relevant part of the queue
    num_events = 0;
    event_dos_moved = false;
    event_dos_wheel = false;

    // The overflow reason is most likely
    queue_overflowed = false;

    if (IsEmpty()) {
        timer_in_progress = false;
        PIC_RemoveEvents(MouseQueueTick);
    }
}

void MouseQueue::StartTimer()
{
    // XXX implement shorter delays for events from different sources at the same time
    // XXX implement different delays based on rate_hz, from both DOS and PS/2 interfaces

    if (!IsEmpty() && !timer_in_progress) {
        timer_in_progress = true;
        PIC_AddEvent(MouseQueueTick, 5.0); // 200 Hz
    }
}

void MouseQueue::Tick()
{
    timer_in_progress = false;

    // If there is no DOS callback registered, don't bother
    // with button events for DOS driver
    if (!mouse_active.dos) {
        num_events = 0;
        event_dos_wheel = 0;
        // moving should still be reported, for updating cursor
    }

    if (!IsEmpty())
        PIC_ActivateIRQ(12);
}


// ***************************************************************************
// Interrupt 74 implementation
// ***************************************************************************

static uintptr_t INT74_Exit()
{
    SegSet16(cs, RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
    reg_ip = RealOff(CALLBACK_RealPointer(int74_ret_callback));

    return CBRET_NONE;
}

static uintptr_t INT74_Handler()
{
    MouseEvent event;
    queue.FetchEvent(event);

    // If DOS driver is active, use it to handle the event
    if (event.req_dos && mouse_active.dos) {

        // Taken from DOSBox X: HERE within the IRQ 12 handler is the
        // appropriate place to redraw the cursor. OSes like Windows 3.1
        // expect real-mode code to do it in response to IRQ 12, not
        // "out of the blue" from the SDL event handler like the
        // original DOSBox code did it. Doing this allows the INT 33h
        // emulation to draw the cursor while not causing Windows 3.1 to
        // crash or behave erratically.
        MOUSEDOS_DrawCursor();

        // If DOS driver's client is not interested in this particular
        // type of event - skip it
        if (!MOUSEDOS_HasCallback(event.event_id))
            return INT74_Exit();
    
        CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
        CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)) + 7);

        return MOUSEDOS_DoCallback(event.event_id, event.buttons_12S);
    }

    // If event is relevant for PS/2, it has to update data packet
    if (event.req_ps2) {
        MOUSEPS2_UpdatePacket();
    }

    // If BIOS interface is active, use it to handle the event
    if (event.req_ps2 && mouse_active.bios) {
        
        CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
        CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)));

        // XXX shouldn't we clear the event queue here?

        return MOUSEBIOS_DoCallback();
    }

    // No mouse emulation module is interested in event
    return INT74_Exit();
}

uintptr_t INT74_Ret_Handler()
{
    queue.StartTimer();
    return CBRET_NONE;
}

// ***************************************************************************
// Helper functions
// ***************************************************************************

static MouseEventId SelectIdPressed(const uint8_t idx, const bool changed_12S)
{
    switch (idx) {
    case 0: return MouseEventId::PressedLeft;
    case 1: return MouseEventId::PressedRight;
    case 2: return MouseEventId::PressedMiddle;
    case 3:
    case 4:
        return changed_12S ? MouseEventId::PressedMiddle
                           : MouseEventId::NotDosEvent;
    default: return MouseEventId::NotDosEvent;
    }
}

static MouseEventId SelectIdReleased(const uint8_t idx, const bool changed_12S)
{
    switch (idx) {
    case 0: return MouseEventId::ReleasedLeft;
    case 1: return MouseEventId::ReleasedRight;
    case 2: return MouseEventId::ReleasedMiddle;
    case 3:
    case 4:
        return changed_12S ? MouseEventId::ReleasedMiddle
                           : MouseEventId::NotDosEvent;
    default: return MouseEventId::NotDosEvent;
    }
}

// ***************************************************************************
// External notifications
// ***************************************************************************

void MOUSE_SetSensitivity(const int32_t new_sensitivity_x,
                          const int32_t new_sensitivity_y)
{
    auto adapt = [](const int32_t sensitivity) {
        constexpr float min = 0.01f;
        constexpr float max = 100.0f;

        const float tmp = std::clamp(static_cast<float>(sensitivity) / 100.0f,
                                     -max, max);

        if (tmp >= 0)
            return std::max(tmp, min);
        else
            return std::min(tmp, -min);
    };

    sensitivity_x = adapt(new_sensitivity_x);
    sensitivity_y = adapt(new_sensitivity_y);
}

void MOUSE_NewScreenParams(const uint16_t clip_x, const uint16_t clip_y,
                           const uint16_t res_x, const uint16_t res_y,
                           const bool fullscreen, const uint16_t x_abs,
                           const uint16_t y_abs)
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

void MOUSE_NotifyDosReset()
{
    queue.ClearDos();
}

void MOUSE_NotifyDriverChanged()
{
	// TODO: there is too much seamless-related logic in sdlmain.cpp;
	// this should be handled within mouse.cpp, as here we have much
	// better knowledge about what mouse interfaces are being used.
	// Besides, if other GUIs are introduces in the future, logic
	// from mouse.cpp can be easily reused.
	
    mouse_seamless_driver = mouse_active.vmware;
    GFX_UpdateMouseState();

    MOUSEPS2_UpdateButtonSquish();
}

void MOUSE_EventMoved(const int16_t x_rel, const int16_t y_rel,
                      const uint16_t x_abs, const uint16_t y_abs)
{
    // From the GUI we are getting mouse movement data in two
    // distinct formats:
    //
    // - relative; this one has a chance to be raw movements,
    //   it has to be fed to PS/2 mouse emulation, serial port
    //   mouse emulation, etc.; any guest side software accessing
    //   these mouse interfaces will most likely implement it's
    //   own mouse acceleration/smoothing/etc.
    // - absolute; this follows host OS mouse behavior and should
    //   be fed to VMware seamless mouse emulation and similar
    //   interfaces
    //
    // Our DOS mouse driver (INT 33h)is a bit special, as it can
    // act both ways (seamless and non-seamless mouse pointer),
    // so it needs data in both formats.
    //
    // Our own sensitivity settings should ONLY be applied to
    // relative mouse movement - applying it to absolute data
    // would have broken the mouse pointer integration

    // Adapt relative movement - use sensitivity settings

    auto x_sen = static_cast<float>(x_rel) * sensitivity_x;
    auto y_sen = static_cast<float>(y_rel) * sensitivity_y;

    // Make sure nothing crosses int16_t value even when added
    // to internally stored sub-pixel movement 

    static constexpr float max = 16380.0f;

    x_sen = std::clamp(x_sen, -max, max);
    y_sen = std::clamp(y_sen, -max, max);

    // Notify mouse interfaces

    MouseEvent event(MouseEventId::MouseHasMoved);

    if (mouse_is_captured) {
        MOUSESERIAL_NotifyMoved(x_sen, y_sen);
        event.req_ps2 = MOUSEPS2_NotifyMoved(x_sen, y_sen);
    }
    event.req_vmm = MOUSEVMWARE_NotifyMoved(x_abs, y_abs);
    event.req_dos = MOUSEDOS_NotifyMoved(x_sen, y_sen, x_abs, y_abs);

    queue.AddEvent(event);
}

void MOUSE_NotifyMovedFake()
{
    MouseEvent event(MouseEventId::MouseHasMoved);
    event.req_vmm = true;

    queue.AddEvent(event);
}

void MOUSE_EventPressed(uint8_t idx)
{
    const auto buttons_12S_old = GetButtonsSquished();

    switch (idx) {
    case 0: // left button
        if (buttons_12.left) return;
        buttons_12.left = 1;
        break;
    case 1: // right button
        if (buttons_12.right) return;
        buttons_12.right = 1;
        break;
    case 2: // middle button
        if (buttons_345.middle) return;
        buttons_345.middle = 1;
        break;
    case 3: // extra button #1
        if (buttons_345.extra_1) return;
        buttons_345.extra_1 = 1;
        break;
    case 4: // extra button #2
        if (buttons_345.extra_2) return;
        buttons_345.extra_2 = 1;
        break;
    default: // button not supported
        return;
    }

    const auto buttons_12S = GetButtonsSquished();
    const bool changed_12S = (buttons_12S_old.data != buttons_12S.data);
    const uint8_t idx_12S  = idx < 2 ? idx : 2;

    MouseEvent event(SelectIdPressed(idx, changed_12S));

    // XXX: 'changed_12S' should not be needed, each emultaion code
    // should track button state by itself

    if (mouse_is_captured) {
        if (changed_12S) {
            MOUSESERIAL_NotifyPressed(buttons_12S, idx_12S);
        }
        event.req_ps2 = MOUSEPS2_NotifyPressedReleased(buttons_12S, GetButtonsJoined());
    }
    if (changed_12S) {
        event.req_vmm = MOUSEVMWARE_NotifyPressedReleased(buttons_12S);
        event.req_dos = MOUSEDOS_NotifyPressed(buttons_12S, idx_12S);
        MOUSESERIAL_NotifyPressed(buttons_12S, idx_12S);
    }

    queue.AddEvent(event);
}

void MOUSE_EventReleased(uint8_t idx)
{
    const auto buttons_12S_old = GetButtonsSquished();

    switch (idx) {
    case 0: // left button
        if (!buttons_12.left) return;
        buttons_12.left = 0;
        break;
    case 1: // right button
        if (!buttons_12.right) return;
        buttons_12.right = 0;
        break;
    case 2: // middle button
        if (!buttons_345.middle) return;
        buttons_345.middle = 0;
        break;
    case 3: // extra button #1
        if (!buttons_345.extra_1) return;
        buttons_345.extra_1 = 0;
        break;
    case 4: // extra button #2
        if (!buttons_345.extra_2) return;
        buttons_345.extra_2 = 0;
        break;
    default: // button not supported
        return;
    }

    const auto buttons_12S = GetButtonsSquished();
    const bool changed_12S = (buttons_12S_old.data != buttons_12S.data);
    const uint8_t idx_12S  = idx < 2 ? idx : 2;

    MouseEvent event(SelectIdReleased(idx, changed_12S));

    // XXX: 'changed_12S' should not be needed, each emultaion code
    // should track button state by itself

    // XXX: NotifyPressedReleased is not a good idea

    // Pass mouse release to all the mice even if host pointer is not captured,
    // to prevent strange effects when pointer goes back in the window

    event.req_ps2 = MOUSEPS2_NotifyPressedReleased(buttons_12S, GetButtonsJoined());
    if (changed_12S) {
        event.req_vmm = MOUSEVMWARE_NotifyPressedReleased(buttons_12S);
        event.req_dos = MOUSEDOS_NotifyReleased(buttons_12S, idx_12S);
        MOUSESERIAL_NotifyReleased(buttons_12S, idx_12S);
    }

    queue.AddEvent(event);
}

void MOUSE_EventWheel(const int16_t w_rel)
{
    MouseEvent event(MouseEventId::WheelHasMoved);

    if (mouse_is_captured) {
        event.req_ps2 = MOUSEPS2_NotifyWheel(w_rel);
        MOUSESERIAL_NotifyWheel(w_rel);
    }

    event.req_vmm = MOUSEVMWARE_NotifyWheel(w_rel);
    event.req_dos = MOUSEDOS_NotifyWheel(w_rel);

    queue.AddEvent(event);
}

// ***************************************************************************
// Initialization
// ***************************************************************************

void MOUSE_Init(Section * /*sec*/)
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

    MOUSEPS2_Init();
    MOUSEVMWARE_Init();
    MOUSEDOS_Init();
}
