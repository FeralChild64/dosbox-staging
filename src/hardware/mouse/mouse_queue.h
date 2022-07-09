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

#ifndef DOSBOX_MOUSE_QUEUE_H
#define DOSBOX_MOUSE_QUEUE_H

#include "mouse_common.h"


class MouseQueue final {
public:

    static MouseQueue &GetInstance();

    void SetRateDOS(const uint16_t rate_hz);
    void SetRatePS2(const uint16_t rate_hz);

    void AddEvent(MouseEvent &event);
    void FetchEvent(MouseEvent &event);
    void ClearEventsDOS();
    void StartTimerIfNeeded();

private:

    MouseQueue()  = default;
    ~MouseQueue() = delete;
    MouseQueue(const MouseQueue &)            = delete;
    MouseQueue &operator=(const MouseQueue &) = delete;

    void Tick();
    friend void mouse_queue_tick(uint32_t);

    void AggregateEventsDOS(MouseEvent &event);
    MouseEvent &PopEventButton();
    void UpdateDelayCounters();
    uint8_t ClampStartDelay(float value_ms) const;

    struct { // intial value of delay counters, in milliseconds
        uint8_t ps2_ms             = 5;
        uint8_t dos_button_ms      = 1;
        uint8_t dos_moved_wheel_ms = 5;
    } start_delay = {};

    // A 'modulo' queue of events
    static constexpr uint8_t event_queue_size = 10;
    std::array<MouseEvent, event_queue_size> events = {};
    uint8_t idx_first  = 0; // index of the first event
    uint8_t num_events = 0; // number of events in the queue

    bool queue_overflow    = false;
    bool timer_in_progress = false;

    // Time in milliseconds which has to elapse before event can take place
    struct {
        uint8_t ps2_ms             = 0;
        uint8_t dos_button_ms      = 0;
        uint8_t dos_moved_wheel_ms = 0;
    } delay = {};

    // Events for which we do not need a queue (alway aggregated)
    bool event_ps2       = false;
    bool event_dos_moved = false;
    bool event_dos_wheel = false;

    MouseButtons12S payload_dos_buttons = 0;

    uint32_t pic_ticks_start = 0; // PIC_Ticks value when timer starts

    // Helpers to check if there are events in the queue
    bool HasEventDosMoved() const;
    bool HasEventDosButton() const;
    bool HasEventDosAny() const;
    bool HasEventPS2() const;
    bool HasEventAny() const;

    // Helpers to check if there are events ready to be handled
    bool HasReadyEventPS2() const;
    bool HasReadyEventDosMoved() const;
    bool HasReadyEventDosButton() const;
    bool HasReadyEventAny() const;
};

#endif // DOSBOX_MOUSE_QUEUE_H
