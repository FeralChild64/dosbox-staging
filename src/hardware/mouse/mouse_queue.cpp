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

#include "mouse_queue.h"

#include <array>

#include "checks.h"
#include "pic.h"

CHECK_NARROWING();


// ***************************************************************************
// Debug code, normally not enabled
// ***************************************************************************

// #define DEBUG_QUEUE_ENABLE

#ifndef DEBUG_QUEUE_ENABLE
#    define DEBUG_QUEUE(...) ;
#else
// TODO: after migrating to C++20, allow to skip the 2nd argument by using
// '__VA_OPT__(,) __VA_ARGS__' instead of ', __VA_ARGS__'
#    define DEBUG_QUEUE(fmt, ...) \
        LOG_INFO("(queue) %04d: " fmt, DEBUG_GetDiffTicks(), __VA_ARGS__);

static uint32_t DEBUG_GetDiffTicks()
{
    static uint32_t previous_ticks = 0;
    uint32_t diff_ticks = 0;

    if (previous_ticks)
        diff_ticks = PIC_Ticks - previous_ticks;

    previous_ticks = PIC_Ticks;
    return diff_ticks;
}

#endif

// ***************************************************************************
// Mouse event queue implementation
// ***************************************************************************

void mouse_queue_tick(uint32_t)
{
    MouseQueue::GetInstance().Tick();
}

MouseQueue &MouseQueue::GetInstance()
{
    static MouseQueue *instance = nullptr;
    if (!instance)
        instance = new MouseQueue();
    return *instance;
}

uint8_t MouseQueue::ClampStartDelay(float value_ms) const
{
    constexpr long min_ms = 3;   // 330 Hz sampling rate
    constexpr long max_ms = 100; //  10 Hz sampling rate

    const auto tmp = std::clamp(std::lround(value_ms), min_ms, max_ms);
    return static_cast<uint8_t>(tmp);
}

void MouseQueue::SetRateDOS(const uint16_t rate_hz)
{
    // Convert rate in Hz to delay in milliseconds
    const float rate_ms = 1000.0f / rate_hz;
    start_delay.dos_moved_wheel_ms = ClampStartDelay(rate_ms);

    // Cheat a little - our delay for buttons is separate and typically
    // smaller, so that button events can be sent to the DOS games with
    // minimal latency. So far this didin't cause any issues.
    start_delay.dos_button_ms = ClampStartDelay(rate_ms / 5.0f);
}

void MouseQueue::SetRatePS2(const uint16_t rate_hz)
{
    // Convert rate in Hz to delay in milliseconds
    start_delay.ps2_ms = ClampStartDelay(1000.0f / rate_hz);
}

void MouseQueue::AddEvent(MouseEvent &event)
{
    DEBUG_QUEUE("AddEvent:   %s %s",
                event.request_ps2 ? "PS2" : "---",
                event.request_dos ? "DOS" : "---");

    // If events are being fetched, clear the DOS overflow flag
    if (mouse_shared.active_dos && !mouse_shared.dos_cb_running)
        queue_overflow = false;

    // If queue got overflowed due to DOS not taking events,
    // don't accept any more DOS events other than mouse or
    // wheel move, as it might lead to strange effects in
    // DOS applications
    if (GCC_UNLIKELY(queue_overflow) && event.request_dos &&
        event.dos_id != MouseEventId::MouseHasMoved &&
        event.dos_id != MouseEventId::WheelHasMoved) {
        event.request_dos = false;
        // Normal mechanism for updating button state is not working now
        payload_dos_buttons = event.dos_buttons;
    }

    // Mouse movements should be aggregated, no point in handling
    // excessive amount of events
    if (event.request_dos)
        AggregateEventsDOS(event);

    // Prevent unnecessary further processing
    if (!event.request_dos && !event.request_ps2)
        return; // event not relevant any more

    bool restart_timer = false;
    if (event.request_ps2) {
        if (!HasEventPS2() && timer_in_progress && !delay.ps2_ms) {
            DEBUG_QUEUE("AddEvent: restart timer for %s", "PS2");
            // We do not want the timer to start only then DOS event
            // gets processed - for minimum latency it is better to
            // restart the timer
            restart_timer = true;
        }

        // Events for PS/2 interfaces (or virtualizer compatible
        // drivers) do not carry any information - they are only
        // notifications that new data is available for fetching
        event_ps2 = true;
    }

    if (event.request_dos) {
        if (!HasEventDosAny() && timer_in_progress &&
            !delay.dos_button_ms && !delay.dos_moved_wheel_ms) {
            DEBUG_QUEUE("AddEvent: restart timer for %s", "DOS");
            // We do not want the timer to start only then PS/2
            // event gets processed - for minimum latency it is
            // better to restart the timer
            restart_timer = true;
        }

        if (event.dos_id == MouseEventId::MouseHasMoved) {
            // Mouse has moved - put in priority place
            event_dos_moved = true;
        } else if (event.dos_id == MouseEventId::WheelHasMoved) {
            // Wheel has moved - put in priority place
            event_dos_wheel = true;
        } else if (GCC_UNLIKELY(num_events >= events.size())) {
            // No space left, queue overflow. Clear it (leave only
            // movement notifications) and don't accept any more
            // button/wheel events until application starts to react
            num_events      = 0;
            event_dos_wheel = false;
            queue_overflow  = true;
        } else {
            // Button press/release - put into the queue
            const auto idx = static_cast<uint8_t>(idx_first + num_events++) %
                events.size();
            events[idx] = event;
        }
    }

    if (restart_timer) {
        timer_in_progress = false;
        PIC_RemoveEvents(mouse_queue_tick);
        UpdateDelayCounters();
        StartTimerIfNeeded();
    } else if (!timer_in_progress) {
        DEBUG_QUEUE("ActivateIRQ, in %s", __FUNCTION__);
        // If no timer in progress, handle the event now
        PIC_ActivateIRQ(12);
    }
}

void MouseQueue::AggregateEventsDOS(MouseEvent &event)
{
    // Try to aggregate move and wheel events
    if ((event_dos_moved && event.dos_id == MouseEventId::MouseHasMoved) ||
        (event_dos_wheel && event.dos_id == MouseEventId::WheelHasMoved)) {
        event.request_dos = false; // DOS queue already has such an event
        return;
    }

    // Non-button events can't be aggregated with button events
    // at this moment, this is only possible once they are being
    // passed to the interrupt
    if (event.dos_id == MouseEventId::MouseHasMoved ||
        event.dos_id == MouseEventId::WheelHasMoved)
        return;

    // Generate masks to detect whether two button events can be
    // aggregated (might be needed later even if we have no more
    // events now)
    if (event.dos_mask & mouse_mask::button_pressed)
        // Set 'pressed+released' for every 'pressed' bit
        event.aggregate_mask = static_cast<uint8_t>(event.dos_mask |
                                                    (event.dos_mask << 1));
    else if (event.dos_mask & mouse_mask::button_released)
        // Set 'pressed+released' for every 'released' bit
        event.aggregate_mask = static_cast<uint8_t>(event.dos_mask |
                                                    (event.dos_mask >> 1));

    // If no button events in the queue, skip further processing
    if (!num_events)
        return;

    // Try to aggregate with the last queue event
    auto &last_event = events[static_cast<uint8_t>(idx_first + num_events) %
        events.size()];
    if (GCC_UNLIKELY(last_event.aggregate_mask & event.aggregate_mask))
        return; // no aggregation possible

    // Both events can be aggregated into a single one
    last_event.dos_mask |= event.dos_mask;
    last_event.aggregate_mask |= event.aggregate_mask;
    last_event.dos_buttons = event.dos_buttons;

    // Event aggregated, DOS does not need it any more
    event.request_dos = false;
}

MouseEvent &MouseQueue::PopEventButton()
{
    assert(num_events > 0);

    auto &event = events[idx_first];

    ++idx_first;
    idx_first = static_cast<uint8_t>(idx_first % events.size());
    --num_events;

    return event;
}

void MouseQueue::FetchEvent(MouseEvent &event)
{
    // First try prioritized (move/wheel) DOS events
    if (HasReadyEventDosMoved()) {
        DEBUG_QUEUE("FetchEvent %s", "DosMoved");

        // Set delay before next DOS events
        delay.dos_button_ms      = start_delay.dos_button_ms;
        delay.dos_moved_wheel_ms = start_delay.dos_moved_wheel_ms;
        // Fill in common event information
        event.request_dos = true;
        event.dos_buttons = payload_dos_buttons;
        // Mark which events to handle
        if (event_dos_moved) {
            event.dos_mask |= mouse_mask::mouse_has_moved;
            event_dos_moved = false;
        }
        if (event_dos_wheel) {
            event.dos_mask |= mouse_mask::wheel_has_moved;
            event_dos_wheel = false;
        }
        // If possible, aggregate button events
        if (!HasReadyEventDosButton())
            return;
        const auto &event_button = PopEventButton();
        event.dos_mask     |= event_button.dos_mask;
        payload_dos_buttons = event_button.dos_buttons;
        event.dos_buttons   = payload_dos_buttons;
        return;
    }

    // Try DOS button events
    if (HasReadyEventDosButton()) {
        DEBUG_QUEUE("FetchEvent %s", "DosButton");

        // Set delay before next DOS events
        delay.dos_button_ms = start_delay.dos_button_ms;
        delay.dos_moved_wheel_ms = std::max(delay.dos_moved_wheel_ms,
                                            delay.dos_button_ms);
        // Take event from the queue
        event = PopEventButton();
        payload_dos_buttons = event.dos_buttons;
        return;
    }

    // Now try PS/2 event
    if (HasReadyEventPS2()) {
        DEBUG_QUEUE("FetchEvent %s", "PS2");

        // Set delay before next PS/2 events
        delay.ps2_ms = start_delay.ps2_ms;
        // PS/2 events are really dummy - merely a notification
        // that something has happened and driver has to react
        event.request_ps2 = true;
        event_ps2 = false;
        return;
    }

    // Nothing to provide to interrupt handler,
    // event will stay empty
}

void MouseQueue::ClearEventsDOS()
{
    // Clear DOS relevant part of the queue
    num_events               = 0;
    event_dos_moved          = false;
    event_dos_wheel          = false;
    delay.dos_moved_wheel_ms = 0;
    delay.dos_button_ms      = 0;

    // The overflow reason is most likely gone
    queue_overflow = false;

    if (!HasEventAny()) {
        timer_in_progress = false;
        PIC_RemoveEvents(mouse_queue_tick);
    }
}

void MouseQueue::StartTimerIfNeeded()
{
    // Do nothing if timer is already in progress
    if (timer_in_progress)
        return;

    bool timer_needed = false;
    uint8_t delay_ms  = UINT8_MAX; // dummy delay, will never be used

    if (HasEventPS2() || delay.ps2_ms) {
        timer_needed = true;
        delay_ms     = std::min(delay_ms, delay.ps2_ms);
    }
    if (HasEventDosMoved() || delay.dos_moved_wheel_ms) {
        timer_needed = true;
        delay_ms     = std::min(delay_ms, delay.dos_moved_wheel_ms);
    } else if (HasEventDosButton() || delay.dos_button_ms) {
        // Do not report button before the movement
        timer_needed = true;
        delay_ms     = std::min(delay_ms, delay.dos_button_ms);
    }

    // If queue is empty and all expired, we need no timer
    if (!timer_needed)
        return;

    // Enforce some non-zero delay between events; needed
    // for example if DOS interrupt handler is busy
    delay_ms = std::max(delay_ms, static_cast<uint8_t>(1));

    // Start the timer
    DEBUG_QUEUE("StartTimer, %d", delay_ms);
    pic_ticks_start   = PIC_Ticks;
    timer_in_progress = true;
    PIC_AddEvent(mouse_queue_tick, static_cast<double>(delay_ms));
}

void MouseQueue::UpdateDelayCounters()
{
    const uint32_t tmp = (PIC_Ticks > pic_ticks_start) ? (PIC_Ticks - pic_ticks_start) : 1;
    uint8_t elapsed    = static_cast<uint8_t>(
                std::min(tmp, static_cast<uint32_t>(UINT8_MAX)));
    if (!pic_ticks_start)
        elapsed = 1;

    auto calc_new_delay = [](const uint8_t delay, const uint8_t elapsed) {
        return static_cast<uint8_t>((delay > elapsed) ? (delay - elapsed)
                                                      : 0);
    };

    delay.ps2_ms             = calc_new_delay(delay.ps2_ms, elapsed);
    delay.dos_moved_wheel_ms = calc_new_delay(delay.dos_moved_wheel_ms, elapsed);
    delay.dos_button_ms      = calc_new_delay(delay.dos_button_ms, elapsed);

    pic_ticks_start = 0;
}

void MouseQueue::Tick()
{
    DEBUG_QUEUE("%s", "Tick");

    timer_in_progress = false;
    UpdateDelayCounters();

    // If we have anything to pass to guest side, activate interrupt;
    // otherwise start the timer again
    if (HasReadyEventAny()) {
        DEBUG_QUEUE("ActivateIRQ, in %s", __FUNCTION__);
        PIC_ActivateIRQ(12);
    } else
        StartTimerIfNeeded();
}

bool MouseQueue::HasEventDosMoved() const
{
    return event_dos_moved || event_dos_wheel;
}

bool MouseQueue::HasEventDosButton() const
{
    return (num_events != 0);
}

bool MouseQueue::HasEventDosAny() const
{
    return HasEventDosMoved() || HasEventDosButton();
}

bool MouseQueue::HasEventPS2() const
{
    return event_ps2;
}

bool MouseQueue::HasEventAny() const
{
    return HasEventDosAny() || HasEventPS2();
}

bool MouseQueue::HasReadyEventPS2() const
{
    return HasEventPS2() && !delay.ps2_ms;
}

bool MouseQueue::HasReadyEventDosMoved() const
{
    return HasEventDosMoved() && !delay.dos_moved_wheel_ms &&
           !mouse_shared.dos_cb_running; // callback busy = no new
                                         // event
}

bool MouseQueue::HasReadyEventDosButton() const
{
    return HasEventDosButton() && !delay.dos_button_ms &&
           !mouse_shared.dos_cb_running; // callback busy = no new
                                         // event
}

bool MouseQueue::HasReadyEventAny() const
{
    return HasReadyEventPS2() || HasReadyEventDosMoved() ||
           HasReadyEventDosButton();
}
