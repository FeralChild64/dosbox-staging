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

#ifndef DOSBOX_MOUSE_MANYMOUSE_H
#define DOSBOX_MOUSE_MANYMOUSE_H

#include "mouse.h"
#include "mouse_interfaces.h"

#include <string>
#include <vector>

#include "../libs/manymouse/manymouse.h"


class MousePhysical
{
public:
    MousePhysical(const std::string &name);

    bool IsMapped() const;
    bool IsDisconnected() const;

    MouseInterfaceId GetMappedInterfaceId() const;
    const std::string &GetName() const;

private:

    friend class ManyMouseGlue;

    const std::string name = "";
    MouseInterfaceId mapped_id = MouseInterfaceId::None;
    bool disconnected = false;
};

class ManyMouseGlue final {
public:

    static ManyMouseGlue &GetInstance();

    void RescanIfSafe();
    void ShutdownIfSafe();
    void NotifyConfigAPI(const bool startup);

    bool ProbeForMapping(uint8_t &device_id);
    uint8_t GetIdx(const std::regex &regex);

    void Map(const uint8_t physical_idx,
             const MouseInterfaceId interface_id);

private:

    friend class MouseInterfaceInfoEntry;
    friend class MousePhysicalInfoEntry;

    ManyMouseGlue()  = default;
    ~ManyMouseGlue() = delete;
    ManyMouseGlue(const ManyMouseGlue &)            = delete;
    ManyMouseGlue &operator=(const ManyMouseGlue &) = delete;

    void InitIfNeeded();
    void ShutdownForced();
    void ClearPhysicalMice();
    void Rescan();

    void UnMap(const MouseInterfaceId interface_id);
    void MapFinalize();

    void HandleEvent(const ManyMouseEvent &event,
                     const bool critical_only = false);
    void Tick();
    friend void manymouse_tick(uint32_t);

    bool initialized            = false;
    bool malfunction            = false; // once set to false, will stay false forever
    bool mapping_in_effect      = false;
    bool rescan_blocked_config  = false; // true = rescan blocked due to config API usage
    uint32_t config_api_counter = 0;

    int num_mice = 0;

    std::string driver_name = "";

    std::vector<MousePhysical> physical_devices = {};
    std::vector<int> rel_x = {}; // not yet reported accumulated movements
    std::vector<int> rel_y = {};

    static constexpr uint8_t max_buttons   = 3;
    static constexpr uint8_t max_mice      = UINT8_MAX;
    static constexpr double  tick_interval = 5.0;
};

#endif // DOSBOX_MOUSE_MANYMOUSE_H