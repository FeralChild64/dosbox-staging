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

#ifndef DOSBOX_MOUSE_INTERFACES_H
#define DOSBOX_MOUSE_INTERFACES_H

#include "mouse_common.h"

#include "../hardware/serialport/serialmouse.h"

// ***************************************************************************
// Base mouse interface
// ***************************************************************************

class MouseInterface
{
public:

    static void InitAllInstances();
    static MouseInterface *Get(const MouseInterfaceId interface_id);
    static MouseInterface *GetDOS();
    static MouseInterface *GetPS2();
    static MouseInterface *GetSerial(const uint8_t port_id);
    static MouseInterface *GetBUS();

    virtual void NotifyMoved(MouseEvent &ev,
                             const float x_rel,
                             const float y_rel,
                             const uint16_t x_abs,
                             const uint16_t y_abs) = 0;
    virtual void NotifyButton(MouseEvent &ev,
                              const uint8_t idx,
                              const bool pressed) = 0;
    virtual void NotifyWheel(MouseEvent &ev,
                             const int16_t w_rel) = 0;

    void NotifyInterfaceRate(const uint16_t rate_hz);
    virtual void NotifyBooting();
    void NotifyDisconnect();

    bool IsMapped() const;
    bool IsMapped(const uint8_t device_idx) const;
    bool IsEmulated() const;
    bool IsUsingEvents() const;
    bool IsUsingHostPointer() const;

    MouseInterfaceId GetInterfaceId() const;
    MouseMapStatus GetMapStatus() const;
    uint8_t GetMappedDeviceIdx() const;
    uint8_t GetSensitivityX() const;
    uint8_t GetSensitivityY() const;
    uint16_t GetMinRate() const;
    uint16_t GetRate() const;

    bool ConfigMap(const uint8_t device_idx);
    void ConfigUnMap();

    void ConfigOnOff(const bool enable);
    void ConfigReset();
    void ConfigSetSensitivity(const uint8_t value_x, const uint8_t value_y);
    void ConfigSetSensitivityX(const uint8_t value);
    void ConfigSetSensitivityY(const uint8_t value);
    void ConfigResetSensitivity();
    void ConfigResetSensitivityX();
    void ConfigResetSensitivityY();
    void ConfigSetMinRate(const uint16_t value_hz);
    void ConfigResetMinRate();

    virtual void UpdateConfig();
    virtual void RegisterListener(CSerialMouse &listener_object);
    virtual void UnRegisterListener();

protected:

    static constexpr uint8_t idx_host_pointer = UINT8_MAX;

    MouseInterface(const MouseInterfaceId interface_id,
                   const float sensitivity_predefined);
    virtual ~MouseInterface() = default;
    virtual void Init();

    uint8_t GetInterfaceIdx() const;

    void SetMapStatus(const MouseMapStatus status,
                      const uint8_t device_idx = idx_host_pointer);

    virtual void UpdateRawMapped();
    virtual void UpdateSensitivity();
    virtual void UpdateRate();
    void UpdateButtons(const uint8_t idx, const bool pressed);
    void ResetButtons();

    bool ChangedButtonsJoined() const;
    bool ChangedButtonsSquished() const;

    MouseButtonsAll GetButtonsJoined() const;
    MouseButtons12S GetButtonsSquished() const;

    bool emulated = true;

    float sensitivity_coeff_x = 1.0f; // cached combined sensitivity coefficients
    float sensitivity_coeff_y = 1.0f; // to reduce amount of multiplications

    uint8_t sensitivity_user_x = 0;
    uint8_t sensitivity_user_y = 0;

    uint16_t rate_hz = 0;
    uint16_t min_rate_hz = 0;
    uint16_t interface_rate_hz = 0;

private:

    MouseInterface() = delete;
    MouseInterface(const MouseInterface &) = delete;
    MouseInterface &operator=(const MouseInterface &) = delete;

    const MouseInterfaceId interface_id = MouseInterfaceId::None;

    MouseMapStatus map_status = MouseMapStatus::HostPointer;
    uint8_t        mapped_idx = idx_host_pointer; // index of mapped physical mouse

    MouseButtons12  buttons_12  = 0; // host side buttons 1 (left), 2 (right)
    MouseButtons345 buttons_345 = 0; // host side buttons 3 (middle), 4, and 5

    MouseButtons12  old_buttons_12  = 0; // pre-update values
    MouseButtons345 old_buttons_345 = 0;

    float sensitivity_predefined = 1.0f; // hardcoded for the given interface
};

extern std::vector<MouseInterface *> mouse_interfaces;

#endif // DOSBOX_MOUSE_INTERFACES_H
