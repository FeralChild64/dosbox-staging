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

#include "bios.h"
#include "callback.h"
#include "checks.h"
#include "cpu.h"
#include "dos_inc.h"
#include "int10.h"
#include "pic.h"
#include "regs.h"

// This file implements the DOS virtual mouse driver

// Reference:
// - Ralf Brown's Interrupt List
// - WHEELAPI.TXT from CuteMouse package
// - https://www.stanislavs.org/helppc/int_33.html
// - http://www2.ift.ulaval.ca/~marchand/ift17583/dosints.pdf

#define CURSOR_SIZE_X  16
#define CURSOR_SIZE_Y  16
#define CURSOR_SIZE_XY (CURSOR_SIZE_X * CURSOR_SIZE_Y)

#define GETPOS_X    (static_cast<int16_t>(pos_x) & state.gran_x)
#define GETPOS_Y    (static_cast<int16_t>(pos_y) & state.gran_y)
#define X_MICKEY    8
#define Y_MICKEY    8
#define HIGHESTBIT  (1 << (CURSOR_SIZE_X - 1))
#define NUM_BUTTONS 3

enum class MouseCursor : uint8_t {
    Software = 0,
    Hardware = 1,
    Text     = 2
};

// These values represent hardware state, not driver state

float pos_x             = 0.0f; // absolute pointer position
float pos_y             = 0.0f;
MouseButtons12S buttons = 0;
int16_t wheel           = 0;
uint8_t rate_hz         = 0; // scanning rate, TODO: 0 should disble mouse

static struct { // DOS driver state

    // Structure containing (only!) data which should be
	// saved/restored during task switching

    // DANGER, WILL ROBINSON!
    ///
    // This whole structure can be read or written from the guest side
    // via virtual DOS driver, functions 0x15 / 0x16 / 0x17.
    // Do not put here any array indices, pointers, or anything that
    // can crash the emulator if filled-in incorrectly, or that can
    // be used by malicious code to escape from emulation!

    bool enabled    = false; // TODO: seting to false should disable mouse
    bool cute_mouse = false;

    uint16_t times_pressed[NUM_BUTTONS]   = {0};
    uint16_t times_released[NUM_BUTTONS]  = {0};
    uint16_t last_released_x[NUM_BUTTONS] = {0};
    uint16_t last_released_y[NUM_BUTTONS] = {0};
    uint16_t last_pressed_x[NUM_BUTTONS]  = {0};
    uint16_t last_pressed_y[NUM_BUTTONS]  = {0};
    uint16_t last_wheel_moved_x           = 0;
    uint16_t last_wheel_moved_y           = 0;

    float mickey_x = 0.0f;
    float mickey_y = 0.0f;

    float mickeys_per_px_x = 0.0f;
    float mickeys_per_px_y = 0.0f;
    float pxs_per_mickey_x = 0.0f;
    float pxs_per_mickey_y = 0.0f;

    int16_t gran_x = 0;
    int16_t gran_y = 0;

    int16_t update_region_x[2] = {0};
    int16_t update_region_y[2] = {0};

    uint16_t language = 0; // language for driver messages, unused
    uint8_t mode      = 0;

    // sensitivity
    uint16_t senv_x_val = 0;
    uint16_t senv_y_val = 0;
    uint16_t dspeed_thr = 0; // threshold, in mickeys/s  TODO: should affect mouse movement
    float senv_x = 0.0f;
    float senv_y = 0.0f;

    // mouse position allowed range
    int16_t minpos_x = 0;
    int16_t maxpos_x = 0;
    int16_t minpos_y = 0;
    int16_t maxpos_y = 0;

    // mouse cursor
    uint8_t page       = 0; // cursor display page number
    bool inhibit_draw  = false;
    uint16_t hidden    = 0;
    uint16_t oldhidden = 0;
    int16_t clipx      = 0;
    int16_t clipy      = 0;
    int16_t hot_x      = 0; // cursor hot spot, horizontal
    int16_t hot_y      = 0; // cursor hot spot, vertical
    bool background    = false;
    int16_t backposx   = 0;
    int16_t backposy   = 0;
    uint8_t backData[CURSOR_SIZE_XY] = {0};
    MouseCursor cursor_type = MouseCursor::Software;


    // cursor shape definition
    uint16_t text_and_mask = 0;
    uint16_t text_xor_mask = 0;
    bool user_screen_mask  = false;
    bool user_cursor_mask  = false;
    uint16_t user_def_screen_mask[CURSOR_SIZE_Y] = {0};
    uint16_t user_def_cursor_mask[CURSOR_SIZE_Y] = {0};

    // user callback
    uint16_t sub_mask = 0;
    uint16_t sub_seg  = 0;
    uint16_t sub_ofs  = 0;

} state;

static RealPt uir_callback;

// ***************************************************************************
// Data - default cursor/mask
// ***************************************************************************

static constexpr uint16_t DEFAULT_TEXT_AND_MASK = 0x77FF;
static constexpr uint16_t DEFAULT_TEXT_XOR_MASK = 0x7700;

static uint16_t DEFAULT_SCREEN_MASK[CURSOR_SIZE_Y] = {
	0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x00FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF
};

static uint16_t DEFAULT_CURSOR_MASK[CURSOR_SIZE_Y] = {
	0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000
};

// ***************************************************************************
// Text mode cursor
// ***************************************************************************

// Write and read directly to the screen. Do no use int_setcursorpos (LOTUS123)
extern void WriteChar(uint16_t col, uint16_t row, uint8_t page, uint8_t chr,
                      uint8_t attr, bool useattr);
extern void ReadCharAttr(uint16_t col, uint16_t row, uint8_t page, uint16_t *result);

void RestoreCursorBackgroundText()
{
    if (state.hidden || state.inhibit_draw)
        return;

    if (state.background) {
        WriteChar(state.backposx,
                  state.backposy,
                  real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE),
                  state.backData[0],
                  state.backData[1],
                  true);
        state.background = false;
    }
}

void DrawCursorText()
{
    // Restore Background
    RestoreCursorBackgroundText();

    // Check if cursor in update region
    if ((GETPOS_Y <= state.update_region_y[1]) &&
        (GETPOS_Y >= state.update_region_y[0]) &&
        (GETPOS_X <= state.update_region_x[1]) &&
        (GETPOS_X >= state.update_region_x[0])) {
        return;
    }

    // Save Background
    state.backposx = GETPOS_X >> 3;
    state.backposy = GETPOS_Y >> 3;
    if (state.mode < 2)
        state.backposx >>= 1;

    // use current page (CV program)
    uint8_t page = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE);

    if (state.cursor_type == MouseCursor::Software) {
        uint16_t result;
        ReadCharAttr(state.backposx, state.backposy, page, &result);
        state.backData[0] = (uint8_t)(result & 0xff);
        state.backData[1] = (uint8_t)(result >> 8);
        state.background  = true;
        // Write Cursor
        result = (result & state.text_and_mask) ^ state.text_xor_mask;
        WriteChar(state.backposx,
                  state.backposy,
                  page,
                  (uint8_t)(result & 0xff),
                  (uint8_t)(result >> 8),
                  true);
    } else {
        uint16_t address = page * real_readw(BIOSMEM_SEG, BIOSMEM_PAGE_SIZE);
        address += (state.backposy * real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS) +
                    state.backposx) *
                   2;
        address /= 2;
        uint16_t cr = real_readw(BIOSMEM_SEG, BIOSMEM_CRTC_ADDRESS);
        IO_Write(cr, 0xe);
        IO_Write(cr + 1, (address >> 8) & 0xff);
        IO_Write(cr, 0xf);
        IO_Write(cr + 1, address & 0xff);
    }
}

// ***************************************************************************
// Graphic mode cursor
// ***************************************************************************

static uint8_t gfxReg3CE[9];
static uint8_t index3C4, gfxReg3C5;

static void SaveVgaRegisters()
{
    if (IS_VGA_ARCH) {
        for (uint8_t i = 0; i < 9; i++) {
            IO_Write(0x3CE, i);
            gfxReg3CE[i] = IO_Read(0x3CF);
        }
        /* Setup some default values in GFX regs that should work */
        IO_Write(0x3CE, 3);
        IO_Write(0x3Cf, 0); // disable rotate and operation
        IO_Write(0x3CE, 5);
        IO_Write(0x3Cf, gfxReg3CE[5] & 0xf0); // Force read/write mode 0

        // Set Map to all planes. Celtic Tales
        index3C4 = IO_Read(0x3c4);
        IO_Write(0x3C4, 2);
        gfxReg3C5 = IO_Read(0x3C5);
        IO_Write(0x3C5, 0xF);
    } else if (machine == MCH_EGA) {
        // Set Map to all planes.
        IO_Write(0x3C4, 2);
        IO_Write(0x3C5, 0xF);
    }
}

static void RestoreVgaRegisters()
{
    if (IS_VGA_ARCH) {
        for (uint8_t i = 0; i < 9; i++) {
            IO_Write(0x3CE, i);
            IO_Write(0x3CF, gfxReg3CE[i]);
        }

        IO_Write(0x3C4, 2);
        IO_Write(0x3C5, gfxReg3C5);
        IO_Write(0x3C4, index3C4);
    }
}

static void ClipCursorArea(int16_t &x1, int16_t &x2, int16_t &y1, int16_t &y2,
                           uint16_t &addx1, uint16_t &addx2, uint16_t &addy)
{
    addx1 = addx2 = addy = 0;
    // Clip up
    if (y1 < 0) {
        addy += (-y1);
        y1 = 0;
    }
    // Clip down
    if (y2 > state.clipy) {
        y2 = state.clipy;
    };
    // Clip left
    if (x1 < 0) {
        addx1 += static_cast<uint16_t>(-x1);
        x1 = 0;
    };
    // Clip right
    if (x2 > state.clipx) {
        addx2 = x2 - state.clipx;
        x2    = state.clipx;
    };
}

static void RestoreCursorBackground()
{
    if (state.hidden || state.inhibit_draw || !state.background)
        return;

    SaveVgaRegisters();

	// Restore background
	int16_t x, y;
	uint16_t addx1, addx2, addy;
	uint16_t dataPos = 0;
	int16_t x1       = state.backposx;
	int16_t y1       = state.backposy;
	int16_t x2       = x1 + CURSOR_SIZE_X - 1;
	int16_t y2       = y1 + CURSOR_SIZE_Y - 1;

	ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

	dataPos = addy * CURSOR_SIZE_X;
	for (y = y1; y <= y2; y++) {
		dataPos += addx1;
		for (x = x1; x <= x2; x++) {
			INT10_PutPixel(x, y, state.page, state.backData[dataPos++]);
		};
		dataPos += addx2;
	};
	state.background = false;

    RestoreVgaRegisters();
}

void MOUSEDOS_DrawCursor()
{
    if (state.hidden || state.inhibit_draw)
        return;
    INT10_SetCurMode();
    // In Textmode ?
    if (CurMode->type == M_TEXT) {
        DrawCursorText();
        return;
    }

    // Check video page. Seems to be ignored for text mode.
    // hence the text mode handled above this
    // >>> removed because BIOS page is not actual page in some cases, e.g.
    // QQP games
    //    if (real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE) != state.page)
    //    return;

    // Check if cursor in update region
    /*    if ((GETPOS_X >= state.update_region_x[0]) && (GETPOS_X <=
       state.update_region_x[1]) && (GETPOS_Y >= state.update_region_y[0]) &&
       (GETPOS_Y <= state.update_region_y[1])) { if (CurMode->type==M_TEXT16)
                RestoreCursorBackgroundText();
            else
                RestoreCursorBackground();
            mouse.shown--;
            return;
        }
       */ /*Not sure yet what to do update region should be set to ??? */

    // Get Clipping ranges

    state.clipx = (int16_t)((Bits)CurMode->swidth - 1); // Get from BIOS?
    state.clipy = (int16_t)((Bits)CurMode->sheight - 1);

    /* might be vidmode == 0x13?2:1 */
    int16_t xratio = 640;
    if (CurMode->swidth > 0)
        xratio /= CurMode->swidth;
    if (xratio == 0)
        xratio = 1;

    RestoreCursorBackground();

    SaveVgaRegisters();

    // Save Background
    int16_t x, y;
    uint16_t addx1, addx2, addy;
    uint16_t dataPos = 0;
    int16_t x1       = GETPOS_X / xratio - state.hot_x;
    int16_t y1       = GETPOS_Y - state.hot_y;
    int16_t x2       = x1 + CURSOR_SIZE_X - 1;
    int16_t y2       = y1 + CURSOR_SIZE_Y - 1;

    ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

    dataPos = addy * CURSOR_SIZE_X;
    for (y = y1; y <= y2; y++) {
        dataPos += addx1;
        for (x = x1; x <= x2; x++) {
            INT10_GetPixel(x, y, state.page, &state.backData[dataPos++]);
        };
        dataPos += addx2;
    };
    state.background = true;
    state.backposx   = GETPOS_X / xratio - state.hot_x;
    state.backposy   = GETPOS_Y - state.hot_y;

    // Draw Mousecursor
    dataPos               = addy * CURSOR_SIZE_X;
    const auto screenMask = state.user_screen_mask ? state.user_def_screen_mask
                                                 : DEFAULT_SCREEN_MASK;
    const auto cursorMask = state.user_cursor_mask ? state.user_def_cursor_mask
                                                 : DEFAULT_CURSOR_MASK;
    for (y = y1; y <= y2; y++) {
        uint16_t scMask = screenMask[addy + y - y1];
        uint16_t cuMask = cursorMask[addy + y - y1];
        if (addx1 > 0) {
            scMask <<= addx1;
            cuMask <<= addx1;
            dataPos += addx1;
        };
        for (x = x1; x <= x2; x++) {
            uint8_t pixel = 0;
            // ScreenMask
            if (scMask & HIGHESTBIT)
                pixel = state.backData[dataPos];
            scMask <<= 1;
            // CursorMask
            if (cuMask & HIGHESTBIT)
                pixel = pixel ^ 0x0f;
            cuMask <<= 1;
            // Set Pixel
            INT10_PutPixel(x, y, state.page, pixel);
            dataPos++;
        };
        dataPos += addx2;
    };

    RestoreVgaRegisters();
}

// ***************************************************************************
// DOS driver interface implementation
// ***************************************************************************

static void UpdateDriverActive()
{
    mouse_shared.active_dos = (state.sub_mask != 0);
}

static uint8_t GetResetWheel8bit()
{
    if (!state.cute_mouse) // wheel only available if CuteMouse extensions are active
        return 0;

    const int8_t tmp = std::clamp(wheel,
                                  static_cast<int16_t>(INT8_MIN),
                                  static_cast<int16_t>(INT8_MAX));
    wheel = 0; // reading always int8_t the wheel counter

    // 0xff for -1, 0xfe for -2, etc.
    return (tmp >= 0) ? tmp : 0x100 + tmp;
}

static uint16_t GetResetWheel16bit()
{
    if (!state.cute_mouse) // wheel only available if CuteMouse extensions are active
        return 0;

    const int16_t tmp = wheel;
    wheel = 0; // reading always clears the wheel counter

    // 0xffff for -1, 0xfffe for -2, etc.
    return (tmp >= 0) ? tmp : 0x10000 + tmp;
}

static void SetMickeyPixelRate(const int16_t ratio_x, const int16_t ratio_y)
{
	// According to https://www.stanislavs.org/helppc/int_33-f.html
	// the values should be non-negative (highest bit not set)

    if ((ratio_x > 0) && (ratio_y > 0)) {
        state.mickeys_per_px_x = static_cast<float>(ratio_x) / X_MICKEY;
        state.mickeys_per_px_y = static_cast<float>(ratio_y) / Y_MICKEY;
        state.pxs_per_mickey_x = X_MICKEY / static_cast<float>(ratio_x);
        state.pxs_per_mickey_y = Y_MICKEY / static_cast<float>(ratio_y);
    }
}

static void SetSensitivity(uint16_t px, uint16_t py, uint16_t dspeed_thr)
{
    px         = std::min(static_cast<uint16_t>(100), px);
    py         = std::min(static_cast<uint16_t>(100), py);
    dspeed_thr = std::min(static_cast<uint16_t>(100), dspeed_thr);
    // save values
    state.senv_x_val = px;
    state.senv_y_val = py;
    state.dspeed_thr = dspeed_thr;
    if ((px != 0) && (py != 0)) {
        px--; // Inspired by CuteMouse
        py--; // Although their cursor update routine is far more
              // complex then ours
        state.senv_x = (static_cast<float>(px) * px) / 3600.0f + 1.0f / 3.0f;
        state.senv_y = (static_cast<float>(py) * py) / 3600.0f + 1.0f / 3.0f;
    }
}

static void SetInterruptRate(uint16_t rate_id)
{
    switch (rate_id) {
    case 0:  rate_hz =   0; break; // no events
    case 1:  rate_hz =  30; break;
    case 2:  rate_hz =  50; break;
    case 3:  rate_hz = 100; break;
    default: rate_hz = 200; break; // above 4 is not suported, set max
    }

    if (rate_hz)
        mouse_shared.event_interval_dos = static_cast<uint8_t>(1000 / rate_hz);
}

static void ResetHardware()
{
    wheel = 0;
    SetInterruptRate(4);
    PIC_SetIRQMask(12, false);
}

void MOUSEDOS_BeforeNewVideoMode()
{
    if (CurMode->type != M_TEXT)
        RestoreCursorBackground();
    else
        RestoreCursorBackgroundText();

    state.hidden     = 1;
    state.oldhidden  = 1;
    state.background = false;
}

// FIXME: Does way to much. Many things should be moved to mouse reset one day
void MOUSEDOS_AfterNewVideoMode(const bool setmode)
{
    state.inhibit_draw = false;
    // Get the correct resolution from the current video mode
    uint8_t mode = mem_readb(BIOS_VIDEO_MODE);
    if (setmode && mode == state.mode)
        LOG(LOG_MOUSE, LOG_NORMAL)
        ("New video mode is the same as the old");
    state.gran_x = (int16_t)0xffff;
    state.gran_y = (int16_t)0xffff;
    switch (mode) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x07: {
        state.gran_x = (mode < 2) ? 0xfff0 : 0xfff8;
        state.gran_y = (int16_t)0xfff8;
        Bitu rows    = IS_EGAVGA_ARCH
                             ? real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS)
                             : 24;
        if ((rows == 0) || (rows > 250))
            rows = 25 - 1;
        state.maxpos_y = 8 * (rows + 1) - 1;
        break;
    }
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x0d:
    case 0x0e:
    case 0x13:
        if (mode == 0x0d || mode == 0x13)
            state.gran_x = (int16_t)0xfffe;
        state.maxpos_y = 199;
        break;
    case 0x0f:
    case 0x10: state.maxpos_y = 349; break;
    case 0x11:
    case 0x12: state.maxpos_y = 479; break;
    default:
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Unhandled videomode %X on reset", mode);
        state.inhibit_draw = true;
        return;
    }

    state.mode               = mode;
    state.maxpos_x           = 639;
    state.minpos_x           = 0;
    state.minpos_y           = 0;
    state.hot_x              = 0;
    state.hot_y              = 0;
    state.user_screen_mask   = false;
    state.user_cursor_mask   = false;
    state.text_and_mask      = DEFAULT_TEXT_AND_MASK;
    state.text_xor_mask      = DEFAULT_TEXT_XOR_MASK;
    state.language           = 0;
    state.page               = 0;
    state.dspeed_thr         = 64;
    state.update_region_y[1] = -1; // offscreen
    state.cursor_type        = MouseCursor::Software;
    state.enabled            = true;

    MOUSE_NotifyDosReset();
}

// FIXME: Much too empty, NewVideoMode contains stuff that should be in here
static void Reset()
{
    // Do reset values:
    //  - mouse position (to center of the screen)
    //  - mouse cursor (should be reset and hidden) XXX
    //  - interrupt mask to 0
    //  - mickey pixel rate
    //  - max x/y to current video mode XXX
    // Do not reset the following values:
    // - state.senv_x_val, state.senv_y_val
    // XXX Not sure:
    // - state.dspeed_thr

    MOUSEDOS_BeforeNewVideoMode();
    MOUSEDOS_AfterNewVideoMode(false);
    SetMickeyPixelRate(8, 16);

    state.mickey_x   = 0;
    state.mickey_y   = 0;
    state.cute_mouse = false;

    state.last_wheel_moved_x = 0;
    state.last_wheel_moved_y = 0;

    for (uint16_t idx = 0; idx < NUM_BUTTONS; idx++) {
        state.times_pressed[idx]   = 0;
        state.times_released[idx]  = 0;
        state.last_pressed_x[idx]  = 0;
        state.last_pressed_y[idx]  = 0;
        state.last_released_x[idx] = 0;
        state.last_released_y[idx] = 0;
    }

    pos_x = static_cast<float>((state.maxpos_x + 1) / 2);
    pos_y = static_cast<float>((state.maxpos_y + 1) / 2);

    state.sub_mask = 0;
    
    mouse_shared.dos_cb_running = false;
    
    UpdateDriverActive();
    MOUSE_NotifyDosReset();
}

static void LimitCoordinates()
{
    auto limit = [](float &pos, const int16_t minpos, const int16_t maxpos) {
        const float min = static_cast<float>(minpos);
        const float max = static_cast<float>(maxpos);
        
        pos = std::clamp(pos, min, max);
    };

    // TODO: If the pointer go out of limited coordinates, and we are in
    //       non-captured mode, show host mouse cursor
    
    limit(pos_x, state.minpos_x, state.maxpos_x);
    limit(pos_y, state.minpos_y, state.maxpos_y);
}

static void MoveCursorCaptured(const float x_rel, const float y_rel)
{
    auto calculate_d =[](const float rel, const float pixel_per_mickey, const float senv) {
        float d = rel * pixel_per_mickey;
        if ((fabs(rel) > 1.0f) || (senv < 1.0f))
            d *= senv;
        return d;
    };

    auto update_mickey = [](float &mickey, const float d, const float mickeys_per_pixel) {
        mickey += d * mickeys_per_pixel;
        if (mickey >= 32768.0f)
            mickey -= 65536.0f;
        else if (mickey <= -32769.0f)
            mickey += 65536.0f;    
    };
    
    // Calculate cursor displacement
    const float dx = calculate_d(x_rel, state.pxs_per_mickey_x, state.senv_x);
    const float dy = calculate_d(y_rel, state.pxs_per_mickey_y, state.senv_y);
    
    // Update mickey counters
    update_mickey(state.mickey_x, dx, state.mickeys_per_px_x);
    update_mickey(state.mickey_y, dy, state.mickeys_per_px_y);

    // Apply mouse movement according to our acceleration model
    pos_x += dx;
    pos_y += dy;
}

static void MoveCursorSeamless(const float x_rel, const float y_rel,
                               const uint16_t x_abs, const uint16_t y_abs)
{
    // Do not update mickeys if mouse cursor is not capttured,
    // as this makes games like DOOM behaving strangely

    auto calculate = [](const uint16_t absolute,
                        const uint16_t res,
                        const uint16_t clip) {
        assert(res > 1u);
        return (static_cast<float>(absolute) - clip) / (res - 1);
    };

    // Apply mouse movement to mimic host OS
    float x = calculate(x_abs, mouse_video.res_x, mouse_video.clip_x);
    float y = calculate(y_abs, mouse_video.res_y, mouse_video.clip_y);

    // TODO: this is probably overcomplicated, especially
    // the usage of relative movement - to be investigated

    if (CurMode->type == M_TEXT) {
        pos_x = x * 8;
        pos_x *= real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
        pos_y = y * 8;
        pos_y *= IS_EGAVGA_ARCH ? (real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1) : 25;
    } else if ((state.maxpos_x < 2048) || (state.maxpos_y < 2048) ||
               (state.maxpos_x != state.maxpos_y)) {
        if ((state.maxpos_x > 0) && (state.maxpos_y > 0)) {
            pos_x = x * state.maxpos_x;
            pos_y = y * state.maxpos_y;
        } else {
            pos_x += x_rel;
            pos_y += y_rel;
        }
    } else {
        // Fake relative movement through absolute coordinates
        pos_x += x_rel;
        pos_y += y_rel;
    }
}

bool MOUSEDOS_NotifyMoved(const float x_rel, const float y_rel,
                          const uint16_t x_abs, const uint16_t y_abs)
{
    const auto old_x = GETPOS_X;
    const auto old_y = GETPOS_Y;
    
    if (mouse_is_captured)
        MoveCursorCaptured(x_rel, y_rel);
    else
        MoveCursorSeamless(x_rel, y_rel, x_abs, y_abs);
    
    // Make sure the cursor stays in the range defined by application
    LimitCoordinates();
    
    // Check if interrupt is needed to report updated position
    return (old_x != GETPOS_X || old_y != GETPOS_Y);
}

bool MOUSEDOS_NotifyPressed(const MouseButtons12S new_buttons_12S, const uint8_t idx)
{
    if (idx >= NUM_BUTTONS)
        return false;

    buttons = new_buttons_12S;

    state.times_pressed[idx]++;
    state.last_pressed_x[idx] = GETPOS_X;
    state.last_pressed_y[idx] = GETPOS_Y;

    return true;
}

bool MOUSEDOS_NotifyReleased(const MouseButtons12S new_buttons_12S, const uint8_t idx)
{
    if (idx >= NUM_BUTTONS)
        return false;

    buttons = new_buttons_12S;

    state.times_released[idx]++;
    state.last_released_x[idx] = GETPOS_X;
    state.last_released_y[idx] = GETPOS_Y;

    return true;
}

bool MOUSEDOS_NotifyWheel(const int16_t w_rel)
{
    if (!state.cute_mouse) // wheel only available if CuteMouse extensions are active
        return false;

    const auto tmp = std::clamp(static_cast<int32_t>(w_rel + wheel),
                                static_cast<int32_t>(INT16_MIN),
                                static_cast<int32_t>(INT16_MAX));

    wheel = static_cast<int16_t>(tmp);
    state.last_wheel_moved_x = GETPOS_X;
    state.last_wheel_moved_y = GETPOS_Y;

    return true;
}

static Bitu INT33_Handler()
{
    switch (reg_ax) {
    case 0x00: // MS MOUSE - reset driver and read status
        ResetHardware();
        [[fallthrough]];
    case 0x21: // MS MOUSE v6.0+ - software reset
        reg_ax = 0xffff; // mouse driver installed
        reg_bx = 3; // for 2 buttons return 0xffff
        Reset();
        break;
    case 0x01: // MS MOUSE v1.0+ - show mouse cursor
        if (state.hidden)
            state.hidden--;
        state.update_region_y[1] = -1; // offscreen
        MOUSEDOS_DrawCursor();
        break;
    case 0x02: // MS MOUSE v1.0+ - hide mouse cursor
        if (CurMode->type != M_TEXT)
            RestoreCursorBackground();
        else
            RestoreCursorBackgroundText();
        state.hidden++;
        break;
    case 0x03: // MS MOUSE v1.0+ / CuteMouse - get position and button status
        reg_bl = buttons.data;
        reg_bh = GetResetWheel8bit(); // CuteMouse clears wheel counter too
        reg_cx = GETPOS_X;
        reg_dx = GETPOS_Y;
        break;
    case 0x04: // MS MOUSE v1.0+ - position mouse cursor
    {
        // If position isn't different from current position, don't
        // change it. (position is rounded so numbers get lost when the
        // rounded number is set) (arena/simulation Wolf)
		// XXX simplify this, add lambdas, compare rouding with position retrieval routines
        if ((int16_t)reg_cx >= state.maxpos_x)
            pos_x = static_cast<float>(state.maxpos_x);
        else if (state.minpos_x >= (int16_t)reg_cx)
            pos_x = static_cast<float>(state.minpos_x);
        else if ((int16_t)reg_cx != GETPOS_X)
            pos_x = static_cast<float>(reg_cx);

        if ((int16_t)reg_dx >= state.maxpos_y)
            pos_y = static_cast<float>(state.maxpos_y);
        else if (state.minpos_y >= (int16_t)reg_dx)
            pos_y = static_cast<float>(state.minpos_y);
        else if ((int16_t)reg_dx != GETPOS_Y)
            pos_y = static_cast<float>(reg_dx);

        MOUSEDOS_DrawCursor();
        break;
    }
    case 0x05: // MS MOUSE v1.0+ / CuteMouse - get button press / wheel data
        if (reg_bx == 0xffff && state.cute_mouse) {
            // 'magic' value for checking wheel instead of button
            reg_bx = GetResetWheel16bit();
            reg_cx = state.last_wheel_moved_x;
            reg_dx = state.last_wheel_moved_y;
        } else if (reg_bx < NUM_BUTTONS) {
            // reg_bx - button index
            reg_ax = buttons.data;
            reg_bx = state.times_pressed[reg_bx];
            reg_cx = state.last_pressed_x[reg_bx];
            reg_dx = state.last_pressed_y[reg_bx];
            state.times_pressed[reg_bx] = 0;
        } else {
            // unsupported - try to do something same
            reg_ax = buttons.data;
            reg_bx = 0;
            reg_cx = 0;
            reg_dx = 0;
        }
        break;
    case 0x06: // MS MOUSE v1.0+ / CuteMouse - get button release data / mouse wheel data
        if (reg_bx == 0xffff && state.cute_mouse) {
            // 'magic' value for checking wheel instead of button
            reg_bx = GetResetWheel16bit();
            reg_cx = state.last_wheel_moved_x;
            reg_dx = state.last_wheel_moved_y;
        } else if (reg_bx < NUM_BUTTONS) {
            reg_ax = buttons.data;
            reg_bx = state.times_released[reg_bx];
            reg_cx = state.last_released_x[reg_bx];
            reg_dx = state.last_released_y[reg_bx];
            state.times_released[reg_bx] = 0;
        } else {
            // unsupported - try to do something same
            reg_ax = buttons.data;
            reg_bx = 0;
            reg_cx = 0;
            reg_dx = 0;
        }
        break;
    case 0x07: // MS MOUSE v1.0+ - define horizontal cursor range
        // Lemmings set 1-640 and wants that. iron seeds set 0-640 but doesn't
        // like 640
        // Iron seed works if newvideo mode with mode 13 sets 0-639
        // Larry 6 actually wants newvideo mode with mode 13 to set it
        // to 0-319
        state.minpos_x = std::min((int16_t)reg_cx, (int16_t)reg_dx);
        state.maxpos_x = std::max((int16_t)reg_cx, (int16_t)reg_dx);
        // Battlechess wants this
        pos_x = std::clamp(pos_x, static_cast<float>(state.minpos_x), static_cast<float>(state.maxpos_x));
        // Or alternatively this:
        // pos_x = (state.maxpos_x - state.minpos_x + 1) / 2;
        LOG(LOG_MOUSE, LOG_NORMAL)
        ("Define Hortizontal range min:%d max:%d", state.minpos_x, state.maxpos_x);
        break;
    case 0x08: // MS MOUSE v1.0+ - define vertical cursor range
        // not sure what to take instead of the CurMode (see case 0x07 as well)
        // especially the cases where sheight= 400 and we set it with
        // the mouse_reset to 200 disabled it at the moment. Seems to
        // break syndicate who want 400 in mode 13
        state.minpos_y = std::min((int16_t)reg_cx, (int16_t)reg_dx);
        state.maxpos_y = std::max((int16_t)reg_cx, (int16_t)reg_dx);
        // Battlechess wants this
        pos_y = std::clamp(pos_y, static_cast<float>(state.minpos_y), static_cast<float>(state.maxpos_y));
        // Or alternatively this:
        // pos_y = (state.maxpos_y - state.minpos_y + 1) / 2;
        LOG(LOG_MOUSE, LOG_NORMAL)
        ("Define Vertical range min:%d max:%d", state.minpos_y, state.maxpos_y);
        break;
    case 0x09: // MS MOUSE v3.0+ - define GFX cursor
    {
        PhysPt src = SegPhys(es) + reg_dx;
        MEM_BlockRead(src, state.user_def_screen_mask, CURSOR_SIZE_Y * 2);
        src += CURSOR_SIZE_Y * 2;
        MEM_BlockRead(src, state.user_def_cursor_mask, CURSOR_SIZE_Y * 2);
        state.user_screen_mask = true;
        state.user_cursor_mask = true;
        state.hot_x = std::clamp((int16_t)reg_bx, static_cast<int16_t>(-CURSOR_SIZE_X), static_cast<int16_t>(CURSOR_SIZE_X));
        state.hot_y = std::clamp((int16_t)reg_cx, static_cast<int16_t>(-CURSOR_SIZE_Y), static_cast<int16_t>(CURSOR_SIZE_Y));
        state.cursor_type = MouseCursor::Text;
        MOUSEDOS_DrawCursor();
        break;
    }
    case 0x0a: // MS MOUSE v3.0+ - define text cursor
        // TODO: shouldn't we use MouseCursor::Text, not MouseCursor::Software?
        state.cursor_type  = (reg_bx ? MouseCursor::Hardware : MouseCursor::Software);
        state.text_and_mask = reg_cx;
        state.text_xor_mask = reg_dx;
        if (reg_bx) {
            INT10_SetCursorShape(reg_cl, reg_dl);
            LOG(LOG_MOUSE, LOG_NORMAL)
            ("Hardware Text cursor selected");
        }
        MOUSEDOS_DrawCursor();
        break;
    case 0x27: // MS MOUSE v7.01+ - get screen/cursor masks and mickey counts
        reg_ax = state.text_and_mask;
        reg_bx = state.text_xor_mask;
        [[fallthrough]];
    case 0x0b: // MS MOUSE v1.0+ - read motion data
        reg_cx = static_cast<int16_t>(state.mickey_x);
        reg_dx = static_cast<int16_t>(state.mickey_y);
        state.mickey_x = 0;
        state.mickey_y = 0;
        break;
    case 0x0c: // MS MOUSE v1.0+ - define interrupt subroutine parameters
        state.sub_mask = reg_cx & 0xff;
        state.sub_seg  = SegValue(es);
        state.sub_ofs  = reg_dx;
        UpdateDriverActive();
        break;
    case 0x0d: // MS MOUSE v1.0+ - light pen emulation on
    case 0x0e: // MS MOUSE v1.0+ - light pen emulation off
	    // Both buttons down = pen pressed, otherwise pen considered off-screen
	    // TODO: maybe implement light pen using SDL touch events?
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Mouse light pen emulation not implemented");
        break;
    case 0x0f: // MS MOUSE v1.0+ - define mickey/pixel rate
        SetMickeyPixelRate(reg_cx, reg_dx);
        break;
    case 0x10: // MS MOUSE v1.0+ - define screen region for updating
        state.update_region_x[0] = (int16_t)reg_cx;
        state.update_region_y[0] = (int16_t)reg_dx;
        state.update_region_x[1] = (int16_t)reg_si;
        state.update_region_y[1] = (int16_t)reg_di;
        MOUSEDOS_DrawCursor();
        break;
    case 0x11: // CuteMouse - get mouse capabilities
        reg_ax           = 0x574d; // Identifier for detection purposes
        reg_bx           = 0;      // Reserved capabilities flags
        reg_cx           = 1;      // Wheel is supported
        state.cute_mouse = true; // This call enables CuteMouse extensions
        wheel            = 0;
        // Previous implementation provided Genius Mouse 9.06 function
        // to get number of buttons
        // (https://sourceforge.net/p/dosbox/patches/32/), it was returning
        // 0xffff in reg_ax and number of buttons in reg_bx; I suppose
        // the CuteMouse extensions are more useful
        break;
    case 0x12: // MS MOUSE - set large graphics cursor block
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Large graphics cursor block not implemented");
        break;
    case 0x13: // MS MOUSE v5.0+ - set double-speed threshold
        state.dspeed_thr = (reg_bx ? reg_bx : 64); // XXX constant for default value
        break;
    case 0x14: // MS MOUSE v3.0+ - exchange event-handler
    {
        const auto old_seg  = state.sub_seg;
        const auto old_ofs  = state.sub_ofs;
        const auto old_mask = state.sub_mask;
        // Set new values
        state.sub_mask = reg_cx;
        state.sub_seg  = SegValue(es);
        state.sub_ofs  = reg_dx;
        UpdateDriverActive();
        // Return old values
        reg_cx = old_mask;
        reg_dx = old_ofs;
        SegSet16(es, old_seg);
        break;
    }
    case 0x15: // MS MOUSE v6.0+ - get driver storage space requirements
        reg_bx = sizeof(state);
        break;
    case 0x16: // MS MOUSE v6.0+ - save driver state
        LOG(LOG_MOUSE, LOG_WARN)("Saving driver state...");
        MEM_BlockWrite(SegPhys(es) + reg_dx, &state, sizeof(state));
        break;
    case 0x17: // MS MOUSE v6.0+ - load driver state
        LOG(LOG_MOUSE, LOG_WARN)("Loading driver state...");
        MEM_BlockRead(SegPhys(es) + reg_dx, &state, sizeof(state));
        UpdateDriverActive();
		// FIXME: we should probably fake an event for mouse movement, redraw cursor, etc.
        break;
    case 0x18: // MS MOUSE v6.0+ - set alternate mouse user handler
    case 0x19: // MS MOUSE v6.0+ - set alternate mouse user handler
		LOG(LOG_MOUSE, LOG_WARN)
        ("Alternate mouse user handler not implemented");
        break;
    case 0x1a: // MS MOUSE v6.0+ - set mouse sensitivity
        SetSensitivity(reg_bx, reg_cx, reg_dx);
        break;
    case 0x1b: //  MS MOUSE v6.0+ - get mouse sensitivity
        reg_bx = state.senv_x_val;
        reg_cx = state.senv_y_val;
        reg_dx = state.dspeed_thr;
        break;
    case 0x1c: // MS MOUSE v6.0+ - set interrupt rate
        SetInterruptRate(reg_bx);
        break;
    case 0x1d: // MS MOUSE v6.0+ - set display page number
        state.page = reg_bl;
        break;
    case 0x1e: // MS MOUSE v6.0+ - get display page number
        reg_bx = state.page;
        break;
    case 0x1f: // MS MOUSE v6.0+ - disable mouse driver
        // ES:BX old mouse driver Zero at the moment TODO
        reg_bx = 0;
        SegSet16(es, 0);
        state.enabled = false;
        state.oldhidden = state.hidden;
        state.hidden    = 1;
        // XXX  AX = 001F if successful, FFFF if error (?) , check function 0x20 too
        break;
    case 0x20: // MS MOUSE v6.0+ - enable mouse driver
        state.enabled = true;
        state.hidden  = state.oldhidden;
        break;
    case 0x22: // MS MOUSE v6.0+ - set language for messages
        // 00h = English, 01h = French, 02h = Dutch, 03h = German, 04h =
        // Swedish 05h = Finnish, 06h = Spanish, 07h = Portugese, 08h =
        // Italian
        state.language = reg_bx;
        break;
    case 0x23: // MS MOUSE v6.0+ - get language for messages
        reg_bx = state.language;
        break;
    case 0x24: // MS MOUSE v6.26+ - get mouse information
        reg_bx = 0x805; // driver version 8.05 woohoo
        reg_ch = 0x04;  // PS/2 type
        reg_cl = 0;     // 0 for PS/2 mouse, IRQ for other types
        break;
    case 0x25: // MS MOUSE v6.26+ - get general driver information
        // TODO: According to PC sourcebook reference
        //       Returns:
        //       AH = status
        //         bit 7 driver type: 1=sys 0=com
        //         bit 6: 0=non-integrated 1=integrated mouse driver
        //         bits 4-5: cursor type  00=software text cursor
        //         01=hardware text cursor 1X=graphics cursor bits 0-3:
        //         Function 28 mouse interrupt rate
        //       AL = Number of MDDS (?)
        //       BX = fCursor lock
        //       CX = FinMouse code
        //       DX = fMouse busy
        LOG(LOG_MOUSE, LOG_ERROR)
        ("General driver information not implemented");
        break;
    case 0x26: // MS MOUSE v6.26+ - get maximum virtual coordinates
        reg_bx = (state.enabled ? 0x0000 : 0xffff);
        reg_cx = (uint16_t)state.maxpos_x;
        reg_dx = (uint16_t)state.maxpos_y;
        break;
    case 0x28: // MS MOUSE v7.0+ - set video mode
        // TODO: According to PC sourcebook
        //       Entry:
        //       CX = Requested video mode
        //       DX = Font size, 0 for default
        //       Returns:
        //       DX = 0 on success, nonzero (requested video mode) if not
        LOG(LOG_MOUSE, LOG_ERROR)("Set video mode not implemented");
        break;
    case 0x29: // MS MOUSE v7.0+ - enumerate video modes
        // TODO: According to PC sourcebook
        //       Entry:
        //       CX = 0 for first, != 0 for next
        //       Exit:
        //       BX:DX = named string far ptr
        //       CX = video mode number
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Enumerate video modes not implemented");
        break;
    case 0x2a: // MS MOUSE v7.01+ - get cursor hot spot
        reg_al = (uint8_t)-state.hidden; // Microsoft uses a negative byte
                                         // counter for cursor visibility
        reg_bx = (uint16_t)state.hot_x;
        reg_cx = (uint16_t)state.hot_y;
        reg_dx = 0x04; // PS/2 mouse type
        break;
    case 0x2b: // MS MOUSE v7.0+ - load acceleration profiles
    case 0x2c: // MS MOUSE v7.0+ - get acceleration profiles
    case 0x2d: // MS MOUSE v7.0+ - select acceleration profile
    case 0x2e: // MS MOUSE v8.10+ - set acceleration profile names
    case 0x33: // MS MOUSE v7.05+ - get/switch accelleration profile
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Custom acceleration profiles not implemented");
        break;
    case 0x2f: // MS MOUSE v7.02+ - mouse hardware reset
        LOG(LOG_MOUSE, LOG_ERROR)
        ("INT 33 AX=2F mouse hardware reset not implemented");
        break;
    case 0x30: // MS MOUSE v7.04+ - get/set BallPoint information
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Get/set BallPoint information not implemented");
        break;
    case 0x31: // MS MOUSE v7.05+ - get current minimum/maximum virtual
               // coordinates
        reg_ax = (uint16_t)state.minpos_x;
        reg_bx = (uint16_t)state.minpos_y;
        reg_cx = (uint16_t)state.maxpos_x;
        reg_dx = (uint16_t)state.maxpos_y;
        break;
    case 0x32: // MS MOUSE v7.05+ - get active advanced functions
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Get active advanced functions not implemented");
        break;
    case 0x34: // MS MOUSE v8.0+ - get initialization file
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Get initialization file not implemented");
        break;
    case 0x35: // MS MOUSE v8.10+ - LCD screen large pointer support
        LOG(LOG_MOUSE, LOG_ERROR)
        ("LCD screen large pointer support not implemented");
        break;
    case 0x4d: // MS MOUSE - return pointer to copyright string
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Return pointer to copyright string not implemented");
        break;
    case 0x6d: // MS MOUSE - get version string
        LOG(LOG_MOUSE, LOG_ERROR)("Get version string not implemented");
        break;
    case 0x70: // Mouse Systems - installation check
    case 0x72: // Mouse Systems 7.01+, Genius Mouse 9.06+ - unknown
    case 0x73: // Mouse Systems 7.01+ - get button assignments
        LOG(LOG_MOUSE, LOG_ERROR)("Mouse Sytems mouse extensions not implemented");
        break;
    case 0x53C1: // Logitech CyberMan
        LOG(LOG_MOUSE, LOG_NORMAL)
        ("Mouse function 53C1 for Logitech CyberMan called. Ignored by regular mouse driver.");
        break;
    default:
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Mouse function %04X not implemented", reg_ax);
        break;
    }
    return CBRET_NONE;
}

static uintptr_t MOUSE_BD_Handler()
{
    // the stack contains offsets to register values
    uint16_t raxpt = real_readw(SegValue(ss), reg_sp + 0x0a);
    uint16_t rbxpt = real_readw(SegValue(ss), reg_sp + 0x08);
    uint16_t rcxpt = real_readw(SegValue(ss), reg_sp + 0x06);
    uint16_t rdxpt = real_readw(SegValue(ss), reg_sp + 0x04);

    // read out the actual values, registers ARE overwritten
    uint16_t rax = real_readw(SegValue(ds), raxpt);
    reg_ax       = rax;
    reg_bx       = real_readw(SegValue(ds), rbxpt);
    reg_cx       = real_readw(SegValue(ds), rcxpt);
    reg_dx       = real_readw(SegValue(ds), rdxpt);

    // some functions are treated in a special way (additional registers)
    switch (rax) {
    case 0x09: // Define GFX Cursor
    case 0x16: // Save driver state
    case 0x17: // load driver state
        SegSet16(es, SegValue(ds));
        break;
    case 0x0c: // Define interrupt subroutine parameters
    case 0x14: // Exchange event-handler
        if (reg_bx != 0)
            SegSet16(es, reg_bx);
        else
            SegSet16(es, SegValue(ds));
        break;
    case 0x10: // Define screen region for updating
        reg_cx = real_readw(SegValue(ds), rdxpt);
        reg_dx = real_readw(SegValue(ds), rdxpt + 2);
        reg_si = real_readw(SegValue(ds), rdxpt + 4);
        reg_di = real_readw(SegValue(ds), rdxpt + 6);
        break;
    default: break;
    }

    INT33_Handler();

    // save back the registers, too
    real_writew(SegValue(ds), raxpt, reg_ax);
    real_writew(SegValue(ds), rbxpt, reg_bx);
    real_writew(SegValue(ds), rcxpt, reg_cx);
    real_writew(SegValue(ds), rdxpt, reg_dx);
    switch (rax) {
    case 0x1f: // Disable Mousedriver
        real_writew(SegValue(ds), rbxpt, SegValue(es));
        break;
    case 0x14: // Exchange event-handler
        real_writew(SegValue(ds), rcxpt, SegValue(es));
        break;
    default: break;
    }

    reg_ax = rax;
    return CBRET_NONE;
}

uintptr_t UIR_Handler()
{
    mouse_shared.dos_cb_running = false;
    return CBRET_NONE;
}

bool MOUSEDOS_HasCallback(const MouseEventId event_id)
{
    return state.sub_mask & static_cast<uint8_t>(event_id);
}

uintptr_t MOUSEDOS_DoCallback(const MouseEventId event_id,
                              const MouseButtons12S buttons_12S)
{
    mouse_shared.dos_cb_running = true;

    reg_ax = static_cast<uint8_t>(event_id);
    reg_bl = buttons_12S.data;
    reg_bh = GetResetWheel8bit();
    reg_cx = GETPOS_X;
    reg_dx = GETPOS_Y;
    reg_si = static_cast<int16_t>(state.mickey_x);
    reg_di = static_cast<int16_t>(state.mickey_y);

    CPU_Push16(RealSeg(uir_callback));
    CPU_Push16(RealOff(uir_callback));
    CPU_Push16(state.sub_seg);
    CPU_Push16(state.sub_ofs);

    return CBRET_NONE;
}

void MOUSEDOS_Init()
{
    // Callback for mouse interrupt 0x33
    auto call_int33 = CALLBACK_Allocate();
    // RealPt i33loc = RealMake(CB_SEG + 1,(call_int33 * CB_SIZE) - 0x10);
    RealPt i33loc = RealMake(DOS_GetMemory(0x1) - 1, 0x10);
    CALLBACK_Setup(call_int33, &INT33_Handler, CB_MOUSE, Real2Phys(i33loc), "Mouse");
    // Wasteland needs low(seg(int33))!=0 and low(ofs(int33))!=0
    real_writed(0, 0x33 << 2, i33loc);

    auto call_mouse_bd = CALLBACK_Allocate();
    CALLBACK_Setup(call_mouse_bd, &MOUSE_BD_Handler, CB_RETF8,
                   PhysMake(RealSeg(i33loc), RealOff(i33loc) + 2),
                   "MouseBD");
    // pseudocode for CB_MOUSE (including the special backdoor entry point):
    //    jump near i33hd
    //    callback MOUSE_BD_Handler
    //    retf 8
    //  label i33hd:
    //    callback INT33_Handler
    //    iret

    // Callback for mouse user routine return
    auto call_uir = CALLBACK_Allocate();
    CALLBACK_Setup(call_uir, &UIR_Handler, CB_RETF_CLI, "mouse uir ret");
    uir_callback = CALLBACK_RealPointer(call_uir);

    state.sub_seg = 0x6362;    // magic value
    state.hidden  = 1;         // hide cursor on startup
    state.mode    = UINT8_MAX; // non-existing mode

    ResetHardware();
    Reset();
    SetSensitivity(50, 50, 50);
}