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

#include "mouse_common.h"
#include "mouse_config.h"
#include "mouse_internals.h"

#include "checks.h"
#include "control.h"
#include "setup.h"
#include "support.h"

CHECK_NARROWING();


// TODO - IntelliMouse Explorer emulation is currently deactivated - there is
// probably no way to test it. The IntelliMouse 3.0 software can use it, but
// it seems to require physical PS/2 mouse registers to work correctly,
// and these are not emulated yet.

// #define ENABLE_EXPLORER_MOUSE


MouseConfig     mouse_config;
MousePredefined mouse_predefined;

static std::vector<std::string> list_models_ps2 = {
    "standard",
    "intellimouse",
#ifdef ENABLE_EXPLORER_MOUSE
    "explorer",
#endif
};

static std::vector<std::string> list_models_com = {
    "2button",
    "3button",
    "wheel",
    "msm",
    "2button+msm",
    "3button+msm",
    "wheel+msm",
};

static std::vector<std::string> list_models_bus = {
    "none",
    "bus",
    "inport",
};

static std::vector<std::string> list_rates = {
    "none",
// Commented out values are probably not interesting
// for the end user as "boosted" sampling rate
//  "10",  // PS/2 mouse
//  "20",  // PS/2 mouse
//  "30",  // bus/InPort mouse
    "40",  // PS/2 mouse, approx. limit for 1200 baud serial mouse
//  "50",  // bus/InPort mouse
    "60",  // PS/2 mouse, used by Microsoft Mouse Driver 8.20
    "80",  // PS/2 mouse, approx. limit for 2400 baud serial mouse
    "100", // PS/2 mouse, bus/InPort mouse, used by CuteMouse 2.1b4
    "125", // USB mouse (basic, non-gaming), Bluetooth mouse
    "160", // approx. limit for 4800 baud serial mouse
    "200", // PS/2 mouse, bus/InPort mouse
    "250", // USB mouse (gaming)
    "330", // approx. limit for 9600 baud serial mouse
    "500", // USB mouse (gaming)

// Todays gaming USB mice are capable of even higher sampling
// rates (like 1000 Hz), but such rates are way higher than
// anything DOS games were designed for; most likely such rates
// would only result in emulator slowdowns and compatibility
// issues.
};

static std::vector<std::string> list_bases_bus = {
     "230",
     "234",
     "238",
     "23c",
};

bool MouseConfig::ParseSerialModel(const std::string &model_str,
                                   MouseModelCOM &model,
                                   bool &auto_msm)
{
    if (model_str == list_models_com[0]) {
        model    = MouseModelCOM::Microsoft;
        auto_msm = false;
        return true;
    } else if (model_str == list_models_com[1]) {
        model    = MouseModelCOM::Logitech;
        auto_msm = false;
        return true;
    } else if (model_str == list_models_com[2]) {
        model    = MouseModelCOM::Wheel;
        auto_msm = false;
        return true;
    } else if (model_str == list_models_com[3]) {
        model = MouseModelCOM::MouseSystems;
        auto_msm = false;
        return true;
    } else if (model_str == list_models_com[4]) {
        model    = MouseModelCOM::Microsoft;
        auto_msm = true;
        return true;
    } else if (model_str == list_models_com[5]) {
        model    = MouseModelCOM::Logitech;
        auto_msm = true;
        return true;
    } else if (model_str == list_models_com[6]) {
        model    = MouseModelCOM::Wheel;
        auto_msm = true;
        return true;
    }

    return false;
}

const std::vector<uint16_t> &MouseConfig::GetValidMinRateList()
{
    static std::vector<uint16_t> out_vec = {};

    if (out_vec.empty()) {
        out_vec.push_back(0);

        for (uint8_t i = 1; i < list_rates.size(); i++)
            out_vec.push_back(static_cast<uint16_t>(std::atoi(list_rates[i].c_str())));
    }

    return out_vec;
}

static void config_read(Section *section)
{
    assert(sec);
    const Section_prop *conf = dynamic_cast<Section_prop *>(section);
    assert(conf);

    PropMultiVal *prop_multi = nullptr;

    // Mouse - DOS driver

    mouse_config.mouse_dos_enable = conf->Get_bool("mouse_dos");
    mouse_config.mouse_dos_immediate = conf->Get_bool("mouse_dos_immediate");

    // Mouse - PS/2 AUX port

    std::string prop_str = conf->Get_string("model_ps2");
    if (prop_str == list_models_ps2[0])
        mouse_config.model_ps2 = MouseModelPS2::Standard;
    if (prop_str == list_models_ps2[1])
        mouse_config.model_ps2 = MouseModelPS2::IntelliMouse;
#ifdef ENABLE_EXPLORER_MOUSE
    if (prop_str == list_models_ps2[2])
        mouse_config.model_ps2 = MouseModelPS2::Explorer;
#endif

    // Mouse - serial (COM port) mice

    auto set_model_com = [](const std::string &model_str,
                            MouseModelCOM& model_var,
                            bool &model_auto) {
        if (model_str == list_models_com[0] || model_str == list_models_com[4])
            model_var = MouseModelCOM::Microsoft;
        if (model_str == list_models_com[1] || model_str == list_models_com[5])
            model_var = MouseModelCOM::Logitech;
        if (model_str == list_models_com[2] || model_str == list_models_com[6])
            model_var = MouseModelCOM::Wheel;
        if (model_str == list_models_com[3])
            model_var = MouseModelCOM::MouseSystems;

        if (model_str == list_models_com[4] ||
            model_str == list_models_com[5] ||
            model_str == list_models_com[6])
            model_auto = true;
        else
            model_auto = false;
    };

    prop_str = conf->Get_string("model_com1");
    set_model_com(prop_str,
                  mouse_config.model_com[0],
                  mouse_config.model_com_auto_msm[0]);
    prop_str = conf->Get_string("model_com2");
    set_model_com(prop_str,
                  mouse_config.model_com[1],
                  mouse_config.model_com_auto_msm[1]);
    prop_str = conf->Get_string("model_com3");
    set_model_com(prop_str,
                  mouse_config.model_com[2],
                  mouse_config.model_com_auto_msm[2]);
    prop_str = conf->Get_string("model_com4");
    set_model_com(prop_str,
                  mouse_config.model_com[3],
                  mouse_config.model_com_auto_msm[3]);

    auto set_model_bus = [](const std::string &model_str,
                            MouseModelBus& model_var) {
        if (model_str == list_models_bus[1])
            model_var = MouseModelBus::Bus;
        if (model_str == list_models_bus[2])
            model_var = MouseModelBus::InPort;
    };

    // Mouse - Bus/InPort mouse

    prop_str = conf->Get_string("model_bus");
    set_model_bus(prop_str, mouse_config.model_bus);
    mouse_config.bus_base = conf->Get_hex("busbase");
    mouse_config.bus_irq  = conf->Get_int("busirq");

    // Mouse sensitivity

    auto &array_x = mouse_config.sensitivity_x;
    auto &array_y = mouse_config.sensitivity_y;

    auto &sensitivity_dos_x  = array_x[static_cast<uint8_t>(MouseInterfaceId::DOS)];
    auto &sensitivity_dos_y  = array_y[static_cast<uint8_t>(MouseInterfaceId::DOS)];
    auto &sensitivity_ps2_x  = array_x[static_cast<uint8_t>(MouseInterfaceId::PS2)];
    auto &sensitivity_ps2_y  = array_y[static_cast<uint8_t>(MouseInterfaceId::PS2)];
    auto &sensitivity_com1_x = array_x[static_cast<uint8_t>(MouseInterfaceId::COM1)];
    auto &sensitivity_com1_y = array_y[static_cast<uint8_t>(MouseInterfaceId::COM1)];
    auto &sensitivity_com2_x = array_x[static_cast<uint8_t>(MouseInterfaceId::COM2)];
    auto &sensitivity_com2_y = array_y[static_cast<uint8_t>(MouseInterfaceId::COM2)];
    auto &sensitivity_com3_x = array_x[static_cast<uint8_t>(MouseInterfaceId::COM3)];
    auto &sensitivity_com3_y = array_y[static_cast<uint8_t>(MouseInterfaceId::COM3)];
    auto &sensitivity_com4_x = array_x[static_cast<uint8_t>(MouseInterfaceId::COM4)];
    auto &sensitivity_com4_y = array_y[static_cast<uint8_t>(MouseInterfaceId::COM4)];
    auto &sensitivity_bus_x  = array_x[static_cast<uint8_t>(MouseInterfaceId::BUS)];
    auto &sensitivity_bus_y  = array_y[static_cast<uint8_t>(MouseInterfaceId::BUS)];

    prop_multi = conf->GetMultiVal("sensitivity_dos");
    assert(prop_multi);
    sensitivity_dos_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_dos_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    prop_multi = conf->GetMultiVal("sensitivity_ps2");
    assert(prop_multi);
    sensitivity_ps2_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_ps2_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    prop_multi = conf->GetMultiVal("sensitivity_com1");
    assert(prop_multi);
    sensitivity_com1_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_com1_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    prop_multi = conf->GetMultiVal("sensitivity_com2");
    assert(prop_multi);
    sensitivity_com2_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_com2_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    prop_multi = conf->GetMultiVal("sensitivity_com3");
    assert(prop_multi);
    sensitivity_com3_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_com3_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    prop_multi = conf->GetMultiVal("sensitivity_com4");
    assert(prop_multi);
    sensitivity_com4_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_com4_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    prop_multi = conf->GetMultiVal("sensitivity_bus");
    assert(prop_multi);
    sensitivity_bus_x = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("x"));
    sensitivity_bus_y = static_cast<uint8_t>(prop_multi->GetSection()->Get_int("y"));

    // Mouse sampling rate

    auto &array_rate = mouse_config.min_rate;

    auto &rate_dos  = array_rate[static_cast<uint8_t>(MouseInterfaceId::DOS)];
    auto &rate_ps2  = array_rate[static_cast<uint8_t>(MouseInterfaceId::PS2)];
    auto &rate_com1 = array_rate[static_cast<uint8_t>(MouseInterfaceId::COM1)];
    auto &rate_com2 = array_rate[static_cast<uint8_t>(MouseInterfaceId::COM2)];
    auto &rate_com3 = array_rate[static_cast<uint8_t>(MouseInterfaceId::COM3)];
    auto &rate_com4 = array_rate[static_cast<uint8_t>(MouseInterfaceId::COM4)];
    auto &rate_bus  = array_rate[static_cast<uint8_t>(MouseInterfaceId::BUS)];

    auto set_rate = [](const std::string &rate_str, uint16_t& min_rate) {
        if (rate_str == list_rates[0])
            min_rate = 0;
        else
            min_rate = static_cast<uint16_t>(std::atoi(rate_str.c_str()));
    };

    prop_str = conf->Get_string("min_rate_dos");
    set_rate(prop_str, rate_dos);
    prop_str = conf->Get_string("min_rate_ps2");
    set_rate(prop_str, rate_ps2);
    prop_str = conf->Get_string("min_rate_com1");
    set_rate(prop_str, rate_com1);
    prop_str = conf->Get_string("min_rate_com2");
    set_rate(prop_str, rate_com2);
    prop_str = conf->Get_string("min_rate_com3");
    set_rate(prop_str, rate_com3);
    prop_str = conf->Get_string("min_rate_com4");
    set_rate(prop_str, rate_com4);
    prop_str = conf->Get_string("min_rate_bus");
    set_rate(prop_str, rate_bus);

    // Physical device name patterns

    auto &array_pattern = mouse_config.map_pattern;

    auto &map_pattern_dos  = array_pattern[static_cast<uint8_t>(MouseInterfaceId::DOS)];
    auto &map_pattern_ps2  = array_pattern[static_cast<uint8_t>(MouseInterfaceId::PS2)];
    auto &map_pattern_com1 = array_pattern[static_cast<uint8_t>(MouseInterfaceId::COM1)];
    auto &map_pattern_com2 = array_pattern[static_cast<uint8_t>(MouseInterfaceId::COM2)];
    auto &map_pattern_com3 = array_pattern[static_cast<uint8_t>(MouseInterfaceId::COM3)];
    auto &map_pattern_com4 = array_pattern[static_cast<uint8_t>(MouseInterfaceId::COM4)];

    map_pattern_dos  = conf->Get_string("map_pattern_dos");
    map_pattern_ps2  = conf->Get_string("map_pattern_ps2");
    map_pattern_com1 = conf->Get_string("map_pattern_com1");
    map_pattern_com2 = conf->Get_string("map_pattern_com2");
    map_pattern_com3 = conf->Get_string("map_pattern_com3");
    map_pattern_com4 = conf->Get_string("map_pattern_com4");

    // Start mouse emulation if ready
    mouse_shared.ready_config_mouse = true;
    MOUSE_Startup();
}

static void config_init(Section_prop &secprop)
{
    constexpr auto only_at_start = Property::Changeable::OnlyAtStart;

    Prop_bool     *prop_bool  = nullptr;
    Prop_hex      *prop_hex   = nullptr;
    Prop_int      *prop_int   = nullptr;
    PropMultiVal  *prop_multi = nullptr;
    Prop_string   *prop_str   = nullptr;

    // Mouse enable/disable settings

    prop_bool = secprop.Add_bool("mouse_dos", only_at_start, true);
    assert(prop_bool);
    prop_bool->Set_help("Enable built-in DOS mouse driver.\n"
                        "Notes:\n"
                        "   Disable if you intend to use original MOUSE.COM driver in emulated DOS.\n"
                        "   When guest OS is booted, built-in driver gets disabled automatically.");

    prop_bool = secprop.Add_bool("mouse_dos_immediate", only_at_start, false);
    assert(prop_bool);
    prop_bool->Set_help("Updates mouse movement counters immediately, without waiting for interrupt.\n"
                        "May improve gameplay, especially in fast paced games (arcade, FPS, etc.) - as\n"
                        "for some games it effectively boosts the mouse sampling rate to 1000 Hz, without\n"
                        "increasing interrupt overhead.\n"
                        "Might cause compatibility issues. List of known incompatible games:\n"
                        "   - Ultima Underworld: The Stygian Abyss\n"
                        "   - Ultima Underworld II: Labyrinth of Worlds\n"
                        "Please file a bug with the project if you find another game that fails when\n"
                        "this is enabled, we will update this list.\n");

    // Mouse models

    prop_str = secprop.Add_string("model_ps2", only_at_start, "intellimouse");
    assert(prop_str);
    prop_str->Set_values(list_models_ps2);
    prop_str->Set_help("PS/2 AUX port mouse model:\n"
    // TODO - Add option "none"
                       "   standard:       3 buttons (standard PS/2 mouse).\n"
                       "   intellimouse:   3 buttons + wheel (Microsoft IntelliMouse).\n"
#ifdef ENABLE_EXPLORER_MOUSE
                       "   explorer:       5 buttons + wheel (Microsoft IntelliMouse Explorer).\n"
#endif
                       "Default: intellimouse");

    prop_str = secprop.Add_string("model_com1", only_at_start, "wheel+msm");
    assert(prop_str);
    prop_str->Set_values(list_models_com);
    prop_str->Set_help("COM1 (serial) port mouse model:\n"
                       "   2button:        2 buttons, Microsoft mouse.\n"
                       "   3button:        3 buttons, Logitech mouse, mostly compatible with Microsoft mouse.\n"
                       "   wheel:          3 buttons + wheel, mostly compatible with Microsoft mouse.\n"
                       "   msm:            3 buttons, Mouse Systems mouse, NOT COMPATIBLE with Microsoft mouse.\n"
                       "   2button+msm:    Automatic choice between 2button and msm.\n"
                       "   3button+msm:    Automatic choice between 3button and msm.\n"
                       "   wheel+msm:      Automatic choice between wheel and msm.\n"
                       "Default: wheel+msm\n"
                       "Notes:\n"
                       "   Go to [serial] section to enable/disable COM port mice.");

    prop_str = secprop.Add_string("model_com2", only_at_start, "wheel+msm");
    assert(prop_str);
    prop_str->Set_values(list_models_com);
    prop_str->Set_help("COM2 (serial) port mouse model");

    prop_str = secprop.Add_string("model_com3", only_at_start, "wheel+msm");
    assert(prop_str);
    prop_str->Set_values(list_models_com);
    prop_str->Set_help("COM3 (serial) port mouse model");

    prop_str = secprop.Add_string("model_com4", only_at_start, "wheel+msm");
    assert(prop_str);
    prop_str->Set_values(list_models_com);
    prop_str->Set_help("COM4 (serial) port mouse model");

    prop_str = secprop.Add_string("model_bus", only_at_start, list_models_bus[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_models_bus);
    prop_str->Set_help("Bus mouse model");

    prop_hex = secprop.Add_hex("busbase", only_at_start, 0x23c);
    assert(prop_hex);
    prop_hex->Set_values(list_bases_bus);
    prop_hex->Set_help("The IO base address of the Bus/InPort mouse");

    prop_int = secprop.Add_int("busirq", only_at_start, 5);
    assert(prop_hex);
    prop_int->SetMinMax(2, 5);
    prop_int->Set_help("The IRQ number of the Bus/InPort mouse");

    // Mouse sensitivity

    prop_multi = secprop.AddMultiVal("sensitivity_dos", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("Internal DOS mouse driver sensitivity, 1-99.\n"
                         "Exponential value. Add 10 to double the sensitivity.\n"
                         "Optional second parameter specifies vertical sensitivity (e.g. 40,60).\n"
                         "Sensitivity for any mouse can be chaged using internal command MOUSECTL.\n");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    prop_multi = secprop.AddMultiVal("sensitivity_ps2", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("PS/2 AUX port mouse sensitivity, 1-99.");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    prop_multi = secprop.AddMultiVal("sensitivity_com1", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("COM1 (serial) port mouse sensitivity, 1-99.");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    prop_multi = secprop.AddMultiVal("sensitivity_com2", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("COM2 (serial) port mouse sensitivity, 1-99.");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    prop_multi = secprop.AddMultiVal("sensitivity_com3", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("COM3 (serial) port mouse sensitivity, 1-99.");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    prop_multi = secprop.AddMultiVal("sensitivity_com4", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("COM4 (serial) port mouse sensitivity, 1-99.");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    prop_multi = secprop.AddMultiVal("sensitivity_bus", only_at_start, ",");
    prop_multi->SetValue("50");
    prop_multi->Set_help("Bus/InPort mouse sensitivity, 1-99.");
    prop_int = prop_multi->GetSection()->Add_int("x", only_at_start, 50);
    prop_int->SetMinMax(1, 99);
    prop_int = prop_multi->GetSection()->Add_int("y", only_at_start, 50);
    prop_int->SetMinMax(1, 99);

    // Mouse sampling rate

    prop_str = secprop.Add_string("min_rate_dos", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("Internal DOS mouse driver minimal sampling rate.\n"
                       "Rate might be higher if guest software requests it.\n"
                       "High values increase mouse smoothness and control precission, especially in fast\n"
                       "paced games (arcade, FPS, etc.), but reduces performance a little and can cause\n"
                       "compatibility problems with badly written games/software.\n"
                       "Bluetooth mice and standard USB mice are limited to 125 Hz - use a gaming mouse\n"
                       "for playing, or else higher sampling rates will have no effect.\n"
                       "Minimal sampling rate for any mouse can be chaged using internal command MOUSECTL.\n");

    prop_str = secprop.Add_string("min_rate_ps2", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("PS/2 AUX port mouse minimal sampling rate.");

    prop_str = secprop.Add_string("min_rate_com1", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("COM1 (serial) port mouse minimal sampling rate.");

    prop_str = secprop.Add_string("min_rate_com2", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("COM2 (serial) port mouse minimal sampling rate.");

    prop_str = secprop.Add_string("min_rate_com3", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("COM3 (serial) port mouse minimal sampling rate.");

    prop_str = secprop.Add_string("min_rate_com4", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("COM4 (serial) port mouse minimal sampling rate.");

    prop_str = secprop.Add_string("min_rate_bus", only_at_start, list_rates[0].c_str());
    assert(prop_str);
    prop_str->Set_values(list_rates);
    prop_str->Set_help("Bus/InPort mouse minimal sampling rate.");

    // Physical device name patterns

    prop_str = secprop.Add_string("map_pattern_dos", only_at_start, "");
    assert(prop_str);
    prop_str->Set_help("Pointing device name pattern, to map to internal DOS mouse driver.\n"
                       "Case insensitive. Accepts '?' and '*' wildcards. Empty = use system pointer.\n"
                       "To get pointing device names use internal MOUSECTL.COM tool. One can also use it to change\n"
                       "device mapping in the runtime.\n");

    prop_str = secprop.Add_string("map_pattern_ps2", only_at_start, "");
    assert(prop_str);
    prop_str->Set_help("Pointing device name pattern, to map to PS/2 AUX port mouse.");

    prop_str = secprop.Add_string("map_pattern_com1", only_at_start, "");
    assert(prop_str);
    prop_str->Set_help("Pointing device name pattern, to map to COM1 (serial) port mouse.");

    prop_str = secprop.Add_string("map_pattern_com2", only_at_start, "");
    assert(prop_str);
    prop_str->Set_help("Pointing device name pattern, to map to COM2 (serial) port mouse.");

    prop_str = secprop.Add_string("map_pattern_com3", only_at_start, "");
    assert(prop_str);
    prop_str->Set_help("Pointing device name pattern, to map to COM3 (serial) port mouse.");

    prop_str = secprop.Add_string("map_pattern_com4", only_at_start, "");
    assert(prop_str);
    prop_str->Set_help("Pointing device name pattern, to map to COM4 (serial) port mouse.");
}

void MOUSE_AddConfigSection(const config_ptr_t &conf)
{
    assert(conf);
    Section_prop *sec = conf->AddSection_prop("mouse", &config_read, false);
    assert(sec);
    config_init(*sec);
}
