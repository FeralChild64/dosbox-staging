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
// XXX try to fix CHECK_NARROWING(); problems in mouse_dos_driver.cpp
// XXX implement mouse acceleration (VMware, DOS), adjust SPEEDEQ_DOS, SPEEDEQ_VMM
// XXX test the queue (debug logs?) - possibly use the parameter

// XXX already finished, to be put in PR notes:
//
// User visible changes
// - Windows 3.1 mouse driver https://git.javispedro.com/cgit/vbados.git is now fully working
// - CuteMouse 2.1 (not 2.0!), if started with /O command line option, is now able
//   to use wheel on emulated PS/2 mouse
// - software banging mouse BIOS directly (like Windows 3.x) can now show cursor behavior
//   much more similar original hardware, especially with raw input enabled
// Internal changes
// - large refactoring - BIOS+PS/2 and DOS driver mouse interfaces were put into separate files
// - implemented several cleanups suggested during 'part 1' pull requests, but were too risky
//   and complicated to apply on non-refactored code
// - Windows 3.x + DOS driver mouse stability fix ported from DOSBox X
// - it should be now much harder (if not impossible) to crash emulator using INT33 function 0x17
//   with malicious data, functions 0x05 and 0x06 can't be forced anymore to read out-of-bounds data
// - implemented more BIOS PS/2 mouse functions
// - preparations for more intelligent 'autoseamless' mouse integration
// Notes
// The main goal of this PR is to make the mouse code easier to maintain and extend. All the user
// visible improvements are either positive side effects, or changes which could now be implemented
// easily with the reworked code. I expect the next PR's will be much smaller in size.
// Future
// Some further improvements are possible, but they would better be a part of subsequent pull requests;
// this one is already rather large:
// - cursor behaviour in seamless windowed mode can be improved - it shouldn't disappear
//   on black borders (if window ration is not the same as emulated screen ratio), or when
//   no DOS application / VMware-compatible driver is listening to absolute mouse prsition events
// - original DOS mouse driver has a speed threshold (INT 33 function 0x13), above which the pointer
//   speed is doubled; this should be emulated; also custom mouse acceleration profiles (functions
//   0x2b-0x2e, 0x33) are worth emulating if software if found which can use them

MouseShared mouse_shared;
MouseVideo  mouse_video;

bool mouse_seamless_driver = false;
bool mouse_suggest_show    = false;

static MouseButtons12  buttons_12  = 0; // host side state of buttons 1 (left), 2 (right)
static MouseButtons345 buttons_345 = 0; // host side state of buttons 3 (middle), 4, and 5

static float sensitivity_x = 0.3f; // sensitivity, might depend on the GUI/GFX
static float sensitivity_y = 0.3f; // for scaling all relative mouse movements

static bool raw_input = true; // true = relative input without host OS mouse acceleration

static uintptr_t int74_ret_callback = 0;

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

    MouseEventId id = MouseEventId::NotDosEvent;
	uint8_t mask    = 0;
    MouseButtons12S buttons_12S = 0;
	
	uint8_t aggr_mask = 0; // mask to check if button events can be aggregated

    MouseEvent() {}
    MouseEvent(MouseEventId id) : id(id), mask(static_cast<uint8_t>(id)) {}

} MouseEvent;

static class MouseQueue final {

public:

    MouseQueue();

    void AddEvent(MouseEvent &event);
    void FetchEvent(MouseEvent &event);
    void ClearEventsDOS();
    void StartTimerIfNeeded();
    void Tick();

private:

    MouseQueue(const MouseQueue&) = delete;
    MouseQueue &operator=(const MouseQueue&) = delete;
	
	void AggregateEventsDOS(MouseEvent &event);
    MouseEvent &PopEventBtn();
	void UpdateDelayCounters();

    std::array<MouseEvent, 8> events = {}; // a modulo queue of events
    uint8_t idx_first = 0; // index of the first event
    uint8_t num_event = 0; // number of events in the queue

    MouseButtons12S last_buttons_12S = 0; // last buttons reported

    bool queue_overflow    = false;
    bool timer_in_progress = false;
	
	// Time in milliseconds which has to elapse before event can take place
	uint8_t delay_ps2     = 0;
	uint8_t delay_dos_btn = 0; // for DOS button events
	uint8_t delay_dos_mov = 0; // for DOS move/wheel events

    // Events for which a flag is enough to store them
    bool event_ps2       = false;
    bool event_dos_moved = false;
    bool event_dos_wheel = false;

    bool prefer_ps2 = false; // true = next time prefer PS/2 event
	
	uint32_t ticks_start = 0; // PIC_Ticks value when timer starts
	
	// Helper masks for aggregating DOS button events
	static constexpr uint8_t aggr_mask_pressed =
	    static_cast<uint8_t>(MouseEventId::PressedLeft)  |
	    static_cast<uint8_t>(MouseEventId::PressedRight) |
		static_cast<uint8_t>(MouseEventId::PressedMiddle);
	static constexpr uint8_t aggr_mask_released =
	    static_cast<uint8_t>(MouseEventId::ReleasedLeft)  |
	    static_cast<uint8_t>(MouseEventId::ReleasedRight) |
		static_cast<uint8_t>(MouseEventId::ReleasedMiddle);
	
	// Helpers to check if there are events in the queue
	inline bool HasEventDOSmov() const { return event_dos_moved || event_dos_wheel; }
	inline bool HasEventDOSbtn() const { return (num_event != 0); }
	inline bool HasEventDOSany() const { return HasEventDOSmov() || HasEventDOSbtn(); }
	inline bool HasEventPS2() const { return event_ps2; }
    inline bool HasEventAny() const { return HasEventDOSany() || HasEventPS2(); }

	// Helpers to check if there are events ready to be handled
	inline bool HasReadyEventPS2() const {
		return HasEventPS2() && !delay_ps2;
	}
	inline bool HasReadyEventDOSmov() const
	{
		return HasEventDOSmov() && !delay_dos_mov &&
		       !mouse_shared.dos_cb_running; // callback busy = no new event
	}	
	inline bool HasReadyEventDOSbtn() const
	{
		return HasEventDOSbtn() && !delay_dos_btn &&
		       !mouse_shared.dos_cb_running; // callback busy = no new event
	}
    inline bool HasReadyEventAny() const
    {
        return HasReadyEventPS2() ||
               HasReadyEventDOSmov() ||
               HasReadyEventDOSbtn();
    }

} queue;

static void MouseQueueTick(uint32_t)
{
    queue.Tick();
}

MouseQueue::MouseQueue()
{
}

void MouseQueue::AddEvent(MouseEvent &event)
{
    // If events are being fetched, clear the DOS overflow flag
    if (mouse_shared.active_dos && !mouse_shared.dos_cb_running)
        queue_overflow = false;

    // If queue got overflowed due to DOS not taking events,
    // don't accept any more events other than mouse move, as it might
	// lead to strange effects in DOS application
    if (queue_overflow && event.req_dos &&
	    event.id != MouseEventId::MouseHasMoved) {
        event.req_dos = false;
        last_buttons_12S = event.buttons_12S;
    }

    // Mouse movements should be aggregated, no point in handling
	// excessive amount of events
	if (event.req_dos)
		AggregateEventsDOS(event);

    // Prevent unnecessary further processing
    if (!event.req_dos && !event.req_ps2 && !event.req_vmm)
        return; //event not relevant for any mouse

    bool restart_timer = false;
    if (event.req_ps2 || event.req_vmm) {
		if (!HasEventPS2() && timer_in_progress)
		{
			// We do not want the timer to start only then DOS event
			// gets processed - for minimum latency it is better to
			// restart the timer
			restart_timer = true;
		}

        // Events for PS/2 interfaces (or virtualizer compatible drivers)
        // do not carry any information - they are only notifications
        // that new data is available for fetching
        event_ps2 = true;
    }

    if (event.req_dos) {
		if (!HasEventDOSany() && timer_in_progress)
		{
			// We do not want the timer to start only then PS/2 event
			// gets processed - for minimum latency it is better to
			// restart the timer
			restart_timer = true;
		}		
		
        if (event.id == MouseEventId::MouseHasMoved) {
            // Mouse has moved - put in priority place
            event_dos_moved = true;
        }
        else if (event.id == MouseEventId::WheelHasMoved) {
            // Wheel has moved - put in priority place
            event_dos_wheel = true;
        }
        else if (num_event >= events.size()) {
            // No space left, queue overflow. Clear it (leave only
			// movement notifications) and don't accept any more
			// button/wheel events until application starts to react
            num_event = 0;
			event_dos_wheel = false;
            queue_overflow = true;
            last_buttons_12S = event.buttons_12S;
        }
        else {
            // Button press/release - put into the queue
            const auto idx = (idx_first + num_event) % events.size();
            num_event++;
            event.buttons_12S.data = GetButtonsSquished().data;
            events[idx] = event;
        }
    }
	
	if (restart_timer) {
		timer_in_progress = false;
		PIC_RemoveEvents(MouseQueueTick);
		UpdateDelayCounters();
		StartTimerIfNeeded();
	}
    else if (!timer_in_progress)
		// If no timer in progress, handle the event now
        PIC_ActivateIRQ(12);
}

void MouseQueue::AggregateEventsDOS(MouseEvent &event)
{
	// Try to aggregate move / wheel events
	if ((event_dos_moved && event.id == MouseEventId::MouseHasMoved) ||
		(event_dos_wheel && event.id == MouseEventId::WheelHasMoved)) {
		event.req_dos = false; // DOS queue already has such an event
		return;
	}
	
	// Try to aggregate button events
	if (event.mask & aggr_mask_pressed)
		// Set 'pressed+released' for every 'pressed' bit
		event.aggr_mask = event.mask | (event.mask << 1);
	else if (event.mask & aggr_mask_released)
		// Set 'pressed+released' for every 'released' bit
		event.aggr_mask = event.mask | (event.mask >> 1);
	// Try to aggregate with the last queue event
	if (num_event) {
		auto &last_event = events[(idx_first + num_event) % events.size()];
		if (!(last_event.aggr_mask & event.aggr_mask))
		{
			last_event.mask      |= event.mask;
			last_event.aggr_mask |= event.aggr_mask;
			// Event aggregated with the last one from the queue;
			// DOS does not need it any more
			event.req_dos = false;
		}
	}
}

MouseEvent &MouseQueue::PopEventBtn()
{
	assert(num_event > 0);

	auto &event = events[idx_first];
	idx_first = static_cast<uint8_t>((idx_first + 1) % events.size());
	num_event--;

	return event;
}

void MouseQueue::FetchEvent(MouseEvent &event)
{
	// First try prioritized (move/wheel) DOS events
    if (HasReadyEventDOSmov()) {
		// Set delay before next DOS events
		delay_dos_btn = mouse_shared.start_delay_dos_btn;
		delay_dos_mov = mouse_shared.start_delay_dos_mov;
        // Fill in common event information
        event.req_dos = true;
        event.buttons_12S = last_buttons_12S;
		// Mark which events to handle
        if (event_dos_moved) {
            event.mask |= static_cast<uint8_t>(MouseEventId::MouseHasMoved);
            event_dos_moved = false;
        }
        if (event_dos_wheel) {
            event.mask |= static_cast<uint8_t>(MouseEventId::WheelHasMoved);
            event_dos_wheel = false;
        }
		// If possible, aggregate button events
		if (!HasReadyEventDOSbtn()) return;
        const auto &event_btn = PopEventBtn();
		event.mask |= event_btn.mask;
		last_buttons_12S  = event_btn.buttons_12S;
		event.buttons_12S = last_buttons_12S;		
		return;
	}

    // We should prefer PS/2 events now (as the last was DOS one),
    // but we can't if there is no PS/2 event ready to be handled
    if (!HasReadyEventPS2())
        prefer_ps2 = false;

	// Try DOS button events
    if (HasReadyEventDOSbtn() && !prefer_ps2) {
        // Next time prefer PS/2 events over buttons for DOS driver
        prefer_ps2 = true;
		// Set delay before next DOS events
		delay_dos_btn = mouse_shared.start_delay_dos_btn;
		delay_dos_mov = std::max(delay_dos_mov, delay_dos_btn);
		// Take event from the queue
        event = PopEventBtn();
        last_buttons_12S = event.buttons_12S;
        return;
    }

    // Now try PS/2 event
    if (HasReadyEventPS2()) {
        // Next time prefer DOS event
        prefer_ps2 = false;
		// Set delay before next PS/2 events
		delay_ps2 = mouse_shared.start_delay_ps2;
        // PS/2 events are really dummy - merely a notification
        // that something has happened and driver has to react
        event.req_ps2 = true;
		event_ps2 = false;
        return;
    }

    // Nothing to provide to interrupt handler,
	// event will stay empty
}

void MouseQueue::ClearEventsDOS()
{
    // Clear DOS relevant part of the queue
    num_event = 0;
    event_dos_moved = false;
    event_dos_wheel = false;
	delay_dos_mov   = 0;
	delay_dos_btn   = 0;

    // The overflow reason is most likely gone
    queue_overflow = false;

    if (!HasEventAny()) {
        timer_in_progress = false;
        PIC_RemoveEvents(MouseQueueTick);
    }
}

void MouseQueue::StartTimerIfNeeded()
{
	// Do nothing if timer is already in progress
	if (timer_in_progress)
		return;

	uint8_t delay = UINT8_MAX; // to mark 'not needed'
	if (delay_ps2)
		delay = std::min(delay, delay_ps2);
	if (delay_dos_mov)
		delay = std::min(delay, delay_dos_mov);
	else if (delay_dos_btn)
		delay = std::min(delay, delay_dos_btn);
	// Enforce some non-zero delay between events; needed
	// for example if DOS interrupt handler is busy
	if (HasEventAny() && (delay == UINT8_MAX))
		delay = 1;

	// If queue is empty and all expired, we need no timer
	if (delay == UINT8_MAX)
		return;

	// Start the timer
    ticks_start = PIC_Ticks;
    PIC_AddEvent(MouseQueueTick, static_cast<double>(delay));
}

void MouseQueue::UpdateDelayCounters()
{
	const uint32_t tmp = (PIC_Ticks > ticks_start) ? (PIC_Ticks - ticks_start) : 1;
    uint8_t elapsed = static_cast<uint8_t>(std::min(tmp, static_cast<uint32_t>(UINT8_MAX)));
	if (!ticks_start)
		elapsed = 1;

	auto calc_new_delay = [](const uint8_t delay, const uint8_t elapsed) {
		return static_cast<uint8_t>((delay > elapsed) ? (delay - elapsed) : 0);
	};

	delay_ps2     = calc_new_delay(delay_ps2, elapsed);
	delay_dos_mov = calc_new_delay(delay_dos_mov, elapsed);
	delay_dos_btn = calc_new_delay(delay_dos_btn, elapsed);

	ticks_start = 0;
}

void MouseQueue::Tick()
{
    timer_in_progress = false;
	UpdateDelayCounters();

	// If we have anything to pass to guest side, activate interrupt;
	// otherwise start the timer again
    if (HasReadyEventAny())
        PIC_ActivateIRQ(12);
	else
		StartTimerIfNeeded();
}

// ***************************************************************************
// Mouse ballistics
// ***************************************************************************

float MOUSE_BallisticsPoly(const float x)
{
	// This routine provides a function for mouse ballistics
	//(cursor acceleration), to be reused in various mouse interfaces.
	// Since this is a DOS emulator, the acceleration model is
	// based on a historic PS/2 mouse specification.
	
	// If we don't have raw mouse input, stay with flat profile;
	// in such case the acceleration is already handled by the host OS,
	// adding our own could lead to hard to predict (most likely
	// undesirable) effects
    if (!raw_input)
        return x;
    
    // Normal PS/2 mouse 2:1 scaling algorithm is just a substitution:
    // 0 => 0, 1 => 1, 2 => 1, 3 => 3, 4 => 6, 5 => 9, other x => x * 2
    // and the same for negatives. But we want smooth cursor movement,
    // therefore we use this polynomial (least square regression,
	// 3rd degree, on points -6, -5, ..., 0, ... , 5, 6, here scaled to
	// give f(6.0) = 6.0).
    // Moreover, this model is used not only to implement better PS/2
	// 2:1 scaling - but also everytime we want to apply mouse
	// acceleration by ourselves.
	//
	// Please treat this polynomial as yet another nod to the past,
	// one more small touch of PC computing history :)
    
    if (x >= 6.0f || x <= -6.0f)
        return x;

    constexpr float a = 0.017153417f;
    constexpr float b = 0.382477002f;

    // Optimized polynomial: a*(x^3) + b*(x^1) 
    return x * (a * x * x + b); 
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
    if (event.req_dos && mouse_shared.active_dos) {

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
        if (!MOUSEDOS_HasCallback(event.mask))
            return INT74_Exit();
    
        CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
        CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)) + 7);

        return MOUSEDOS_DoCallback(event.mask, event.buttons_12S);
    }

    // If BIOS interface is active, use it to handle the event
    if (event.req_ps2 && mouse_shared.active_bios) {
        
        CPU_Push16(RealSeg(CALLBACK_RealPointer(int74_ret_callback)));
        CPU_Push16(RealOff(CALLBACK_RealPointer(int74_ret_callback)));

		MOUSEPS2_UpdatePacket();
        return MOUSEBIOS_DoCallback();
    }

    // No mouse emulation module is interested in event
    return INT74_Exit();
}

uintptr_t INT74_Ret_Handler()
{
    queue.StartTimerIfNeeded();
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

void MOUSE_SetConfig(const int32_t new_sensitivity_x,
                     const int32_t new_sensitivity_y,
                     const bool new_raw_input)
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
    raw_input     = new_raw_input;
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

    MOUSEVMM_NewScreenParams(x_abs, y_abs);
}

void MOUSE_NotifyDosReset()
{
    queue.ClearEventsDOS();
}

void MOUSE_NotifyStateChanged()
{
	const auto old_seamless_driver    = mouse_seamless_driver;
	const auto old_mouse_suggest_show = mouse_suggest_show;
	
	// Prepare suggestions to the GUI
    mouse_seamless_driver = mouse_shared.active_vmm &&
                            !mouse_video.fullscreen;
	mouse_suggest_show    = !mouse_shared.active_bios &&
	                        !mouse_shared.active_dos;
	
	// TODO: if active_dos, but mouse pointer is outside of defined
	// range, suggest to show mouse pointer

    // Do not suggest to show host pointer if fullscreen
	// or if autoseamless mode is disabled
	mouse_suggest_show &= !mouse_video.fullscreen;
	mouse_suggest_show &= !mouse_video.autoseamless;
	
	// If state has really changed, update the GUI
    if (mouse_seamless_driver != old_seamless_driver ||
	    mouse_suggest_show    != old_mouse_suggest_show)
        GFX_UpdateMouseState();
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
    auto x_mov = static_cast<float>(x_rel) * sensitivity_x;
    auto y_mov = static_cast<float>(y_rel) * sensitivity_y;

    // Clamp the resulting values to something sane, just in case
    x_mov = std::clamp(x_mov, -MOUSE_REL_MAX, MOUSE_REL_MAX);
    y_mov = std::clamp(y_mov, -MOUSE_REL_MAX, MOUSE_REL_MAX);

    // Notify mouse interfaces

    MouseEvent event(MouseEventId::MouseHasMoved);

    if (!mouse_video.autoseamless || mouse_is_captured) {
        MOUSESERIAL_NotifyMoved(x_mov, y_mov);
        event.req_ps2 = MOUSEPS2_NotifyMoved(x_mov, y_mov);
    }
    event.req_vmm = MOUSEVMM_NotifyMoved(x_mov, y_mov, x_abs, y_abs);
    event.req_dos = MOUSEDOS_NotifyMoved(x_mov, y_mov, x_abs, y_abs);

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

    if (!mouse_video.autoseamless || mouse_is_captured) {
        if (changed_12S) {
            MOUSESERIAL_NotifyPressed(buttons_12S, idx_12S);
        }
        event.req_ps2 = MOUSEPS2_NotifyPressedReleased(buttons_12S, GetButtonsJoined());
    }
    if (changed_12S) {
        event.req_vmm = MOUSEVMM_NotifyPressedReleased(buttons_12S);
        event.req_dos = MOUSEDOS_NotifyPressed(buttons_12S, idx_12S, event.id);
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

    // Pass mouse release to all the mice even if host pointer is not captured,
    // to prevent strange effects when pointer goes back in the window

    event.req_ps2 = MOUSEPS2_NotifyPressedReleased(buttons_12S, GetButtonsJoined());
    if (changed_12S) {
        event.req_vmm = MOUSEVMM_NotifyPressedReleased(buttons_12S);
        event.req_dos = MOUSEDOS_NotifyReleased(buttons_12S, idx_12S, event.id);
        MOUSESERIAL_NotifyReleased(buttons_12S, idx_12S);
    }

    queue.AddEvent(event);
}

void MOUSE_EventWheel(const int16_t w_rel)
{
    MouseEvent event(MouseEventId::WheelHasMoved);

    if (!mouse_video.autoseamless || mouse_is_captured) {
        event.req_ps2 = MOUSEPS2_NotifyWheel(w_rel);
        MOUSESERIAL_NotifyWheel(w_rel);
    }

    event.req_vmm = MOUSEVMM_NotifyWheel(w_rel);
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
    MOUSEVMM_Init();
    MOUSEDOS_Init();
}
