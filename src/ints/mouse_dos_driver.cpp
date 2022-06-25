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

// TODO: Make it passing without errors
// CHECK_NARROWING();

// This file implements the DOS virtual mouse driver

// Reference:
// - https://www.ctyme.com/intr/int-33.htm
// - https://www.stanislavs.org/helppc/int_33.html
// - http://www2.ift.ulaval.ca/~marchand/ift17583/dosints.pdf

#define GETPOS_X    (static_cast<int16_t>(state.x) & state.gran_x)
#define GETPOS_Y    (static_cast<int16_t>(state.y) & state.gran_y)
#define X_CURSOR    16
#define Y_CURSOR    16
#define XY_CURSOR   (X_CURSOR * Y_CURSOR)
#define X_MICKEY    8
#define Y_MICKEY    8
#define HIGHESTBIT  (1 << (X_CURSOR - 1))
#define NUM_BUTTONS 3

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

    bool enabled    = false; // XXX use it
    bool cute_mouse = false;

    // XXX these are hardware state, shouldn't be here!
    float x = 0.0f;
    float y = 0.0f;
    MouseButtons12S buttons = 0;
    int16_t wheel = 0;

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
    int16_t min_x  = 0;
    int16_t max_x  = 0;
    int16_t min_y  = 0;
    int16_t max_y  = 0;

    // mouse cursor
    uint8_t page                = 0; // cursor display page number
    bool inhibit_draw           = false;
    uint16_t hidden             = 0;
    uint16_t oldhidden          = 0;
    bool background             = false;
    int16_t backposx            = 0;
    int16_t backposy            = 0;
    uint8_t backData[XY_CURSOR] = {0};
    int16_t clipx               = 0;
    int16_t clipy               = 0;
    int16_t hot_x               = 0; // cursor hot spot, horizontal
    int16_t hot_y               = 0; // cursor hot spot, vertical
    uint16_t cursor_type        = 0; // o = software, 1 = hardware, 2 = text  XXX introduce enum class

    // cursor shape definition
    uint16_t text_and_mask = 0;
    uint16_t text_xor_mask = 0;
    bool user_screen_mask  = false;
    bool user_cursor_mask  = false;
    uint16_t user_def_screen_mask[Y_CURSOR] = {0};
    uint16_t user_def_cursor_mask[Y_CURSOR] = {0};

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

static uint16_t DEFAULT_SCREEN_MASK[Y_CURSOR] = {
	0x3FFF, 0x1FFF, 0x0FFF, 0x07FF, 0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x00FF, 0x30FF, 0xF87F, 0xF87F, 0xFCFF
};

static uint16_t DEFAULT_CURSOR_MASK[Y_CURSOR] = {
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

    if (state.cursor_type == 0) {
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
	int16_t x2       = x1 + X_CURSOR - 1;
	int16_t y2       = y1 + Y_CURSOR - 1;

	ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

	dataPos = addy * X_CURSOR;
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
    int16_t x2       = x1 + X_CURSOR - 1;
    int16_t y2       = y1 + Y_CURSOR - 1;

    ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

    dataPos = addy * X_CURSOR;
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
    dataPos               = addy * X_CURSOR;
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
    mouse_active.dos = (state.sub_mask != 0);
    // XXX check also status.enabled
}

static uint8_t GetResetWheel8bit()
{
    if (!state.cute_mouse) // wheel only available if CuteMouse extensions are active
        return 0;

    const int8_t tmp = std::clamp(state.wheel,
                                  static_cast<int16_t>(INT8_MIN),
                                  static_cast<int16_t>(INT8_MAX));
    state.wheel = 0; // reading always int8_t the wheel counter

    // 0xff for -1, 0xfe for -2, etc.
    return (tmp >= 0) ? tmp : 0x100 + tmp;
}

static uint16_t GetResetWheel16bit()
{
    if (!state.cute_mouse) // wheel only available if CuteMouse extensions are active
        return 0;

    const int16_t tmp = state.wheel;
    state.wheel = 0; // reading always clears the wheel counter

    // 0xffff for -1, 0xfffe for -2, etc.
    return (tmp >= 0) ? tmp : 0x10000 + tmp;
}

static void SetMickeyPixelRate(const int16_t ratio_x, const int16_t ratio_y)
{
	// XXX according to https://www.stanislavs.org/helppc/int_33-f.html
	// the values should be non-negative (highest bit not set)
    if ((ratio_x != 0) && (ratio_y != 0)) {
        state.mickeys_per_px_x = static_cast<float>(ratio_x) / X_MICKEY;
        state.mickeys_per_px_y = static_cast<float>(ratio_y) / Y_MICKEY;
        state.pxs_per_mickey_x  = X_MICKEY / static_cast<float>(ratio_x);
        state.pxs_per_mickey_y  = Y_MICKEY / static_cast<float>(ratio_y);
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

static void ResetHardware()
{
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
        state.max_y = 8 * (rows + 1) - 1;
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
        state.max_y = 199;
        break;
    case 0x0f:
    case 0x10: state.max_y = 349; break;
    case 0x11:
    case 0x12: state.max_y = 479; break;
    default:
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Unhandled videomode %X on reset", mode);
        state.inhibit_draw = true;
        return;
    }

    state.mode              = mode;
    state.max_x             = 639;
    state.min_x             = 0;
    state.min_y             = 0;
    state.hot_x              = 0;
    state.hot_y              = 0;
    state.user_screen_mask    = false;
    state.user_cursor_mask    = false;
    state.text_and_mask       = DEFAULT_TEXT_AND_MASK;
    state.text_xor_mask       = DEFAULT_TEXT_XOR_MASK;
    state.language          = 0;
    state.page              = 0;
    state.dspeed_thr        = 64;
    state.update_region_y[1] = -1; // offscreen
    state.cursor_type        = 0;
    state.enabled           = true;

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
    state.wheel      = 0;
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

    state.x = static_cast<float>((state.max_x + 1) / 2);
    state.y = static_cast<float>((state.max_y + 1) / 2);

    state.sub_mask = 0;
    
    mouse_active.dos_cb_running = false;
    
    UpdateDriverActive();
    MOUSE_NotifyDosReset();
}

static void LimitCoordinates()
{
    auto limit = [](float &coordinate, const int16_t min, const int16_t max) {
        const float min_float = static_cast<float>(min);
        const float max_float = static_cast<float>(max);
        
        coordinate = std::clamp(coordinate, min_float, max_float);
    };

    // TODO: If the pointer go out of limited coordinates, and we are in
    //       non-captured mode, show host mouse cursor
    
    limit(state.x, state.min_x, state.max_x);
    limit(state.y, state.min_y, state.max_y);
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
    state.x += dx;
    state.y += dy;
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
        state.x = x * 8;
        state.x *= real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
        state.y = y * 8;
        state.y *= IS_EGAVGA_ARCH ? (real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1) : 25;
    } else if ((state.max_x < 2048) || (state.max_y < 2048) ||
               (state.max_x != state.max_y)) {
        if ((state.max_x > 0) && (state.max_y > 0)) {
            state.x = x * state.max_x;
            state.y = y * state.max_y;
        } else {
            state.x += x_rel;
            state.y += y_rel;
        }
    } else {
        // Fake relative movement through absolute coordinates
        state.x += x_rel;
        state.y += y_rel;
    }
}

bool MOUSEDOS_NotifyMoved(const float x_rel, const float y_rel,
                          const uint16_t x_abs, const uint16_t y_abs)
{
    const auto old_x = GETPOS_X;
    const auto old_y = GETPOS_Y;
    
    // XXX move these to subroutines

    if (mouse_is_captured)
        MoveCursorCaptured(x_rel, y_rel);
    else
        MoveCursorSeamless(x_rel, y_rel, x_abs, y_abs);
    
    // Make sure the cursor stays in the range defined by application
    LimitCoordinates();
    
    // Check if interrupt is needed to report updated position
    return (old_x != GETPOS_X || old_y != GETPOS_Y);
}

bool MOUSEDOS_NotifyPressed(const MouseButtons12S buttons_12S, const uint8_t idx)
{
    if (idx >= NUM_BUTTONS)
        return false;

    state.buttons = buttons_12S;

    state.times_pressed[idx]++;
    state.last_pressed_x[idx] = GETPOS_X;
    state.last_pressed_y[idx] = GETPOS_Y;

    return true;
}

bool MOUSEDOS_NotifyReleased(const MouseButtons12S buttons_12S, const uint8_t idx)
{
    if (idx >= NUM_BUTTONS)
        return false;

    state.buttons = buttons_12S;

    state.times_released[idx]++;
    state.last_released_x[idx] = GETPOS_X;
    state.last_released_y[idx] = GETPOS_Y;

    return true;
}

bool MOUSEDOS_NotifyWheel(const int16_t w_rel)
{
    if (!state.cute_mouse) // wheel only available if CuteMouse extensions are active
        return false;

    const auto tmp = std::clamp(static_cast<int32_t>(w_rel + state.wheel),
                                static_cast<int32_t>(INT16_MIN),
                                static_cast<int32_t>(INT16_MAX));

    state.wheel = static_cast<int16_t>(tmp);
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
        // XXX - check once againcursor hiding (0x01, 0x02) - https://www.stanislavs.org/helppc/int_33.html
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
        reg_bl = state.buttons.data;
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
        if ((int16_t)reg_cx >= state.max_x)
            state.x = static_cast<float>(state.max_x);
        else if (state.min_x >= (int16_t)reg_cx)
            state.x = static_cast<float>(state.min_x);
        else if ((int16_t)reg_cx != GETPOS_X)
            state.x = static_cast<float>(reg_cx);

        if ((int16_t)reg_dx >= state.max_y)
            state.y = static_cast<float>(state.max_y);
        else if (state.min_y >= (int16_t)reg_dx)
            state.y = static_cast<float>(state.min_y);
        else if ((int16_t)reg_dx != GETPOS_Y)
            state.y = static_cast<float>(reg_dx);

        MOUSEDOS_DrawCursor();
        break;
    }
    case 0x05: // MS MOUSE v1.0+ / CuteMouse - get button press / wheel data
    {
        uint16_t idx = reg_bx;                   // button index
        if (idx == 0xffff && state.cute_mouse) { // 'magic' index for
                                             // checking wheel
                                             // instead of button
            reg_bx = GetResetWheel16bit();
            reg_cx = state.last_wheel_moved_x;
            reg_dx = state.last_wheel_moved_y;
        } else {
            reg_ax = state.buttons.data;
            if (idx >= NUM_BUTTONS)
                idx = NUM_BUTTONS - 1; // XXX this is certainly wrong!
            reg_cx                   = state.last_pressed_x[idx];
            reg_dx                   = state.last_pressed_y[idx];
            reg_bx                   = state.times_pressed[idx];
            state.times_pressed[idx] = 0;
        }
        break;
    }
    case 0x06: // MS MOUSE v1.0+ / CuteMouse - get button release data / mouse wheel data
    {
        uint16_t idx = reg_bx;                   // button index
        if (idx == 0xffff && state.cute_mouse) { // 'magic' index for
                                             // checking wheel
                                             // instead of button
            reg_bx = GetResetWheel16bit();
            reg_cx = state.last_wheel_moved_x;
            reg_dx = state.last_wheel_moved_y;
        } else {
            reg_ax = state.buttons.data;
            if (idx >= NUM_BUTTONS)
                idx = NUM_BUTTONS - 1; // XXX this is certainly wrong!
            reg_cx                    = state.last_released_x[idx];
            reg_dx                    = state.last_released_y[idx];
            reg_bx                    = state.times_released[idx];
            state.times_released[idx] = 0;
        }
        break;
    }
    case 0x07: // MS MOUSE v1.0+ - define horizontal cursor range
    {
        // Lemmings set 1-640 and wants that. iron seeds set 0-640 but doesn't
        // like 640
        // Iron seed works if newvideo mode with mode 13 sets 0-639
        // Larry 6 actually wants newvideo mode with mode 13 to set it
        // to 0-319
		// XXX simplify this, de-duplicate with 0x08
        int16_t max, min;
        if ((int16_t)reg_cx < (int16_t)reg_dx) {
            min = (int16_t)reg_cx;
            max = (int16_t)reg_dx;
        } else {
            min = (int16_t)reg_dx;
            max = (int16_t)reg_cx;
        }
        state.min_x = min;
        state.max_x = max;
        // Battlechess wants this
        if (state.x > state.max_x)
            state.x = state.max_x;
        if (state.x < state.min_x)
            state.x = state.min_x;
        // Or alternatively this:
        // state.x = (state.max_x - state.min_x + 1) / 2;
        LOG(LOG_MOUSE, LOG_NORMAL)
        ("Define Hortizontal range min:%d max:%d", min, max);
        break;
    }
    case 0x08: // MS MOUSE v1.0+ - define vertical cursor range
    {
        // not sure what to take instead of the CurMode (see case 0x07 as well)
        // especially the cases where sheight= 400 and we set it with
        // the mouse_reset to 200 disabled it at the moment. Seems to
        // break syndicate who want 400 in mode 13
		// XXX simplify this, de-duplicate with 0x08
        int16_t max, min;
        if ((int16_t)reg_cx < (int16_t)reg_dx) {
            min = (int16_t)reg_cx;
            max = (int16_t)reg_dx;
        } else {
            min = (int16_t)reg_dx;
            max = (int16_t)reg_cx;
        }
        state.min_y = min;
        state.max_y = max;
        // Battlechess wants this
        if (state.y > state.max_y)
            state.y = state.max_y;
        if (state.y < state.min_y)
            state.y = state.min_y;
        // Or alternatively this:
        // state.y = (state.max_y - state.min_y + 1) / 2;
        LOG(LOG_MOUSE, LOG_NORMAL)
        ("Define Vertical range min:%d max:%d", min, max);
        break;
    }
    case 0x09: // MS MOUSE v3.0+ - define GFX cursor
    {
        PhysPt src = SegPhys(es) + reg_dx;
        MEM_BlockRead(src, state.user_def_screen_mask, Y_CURSOR * 2);
        src += Y_CURSOR * 2;
        MEM_BlockRead(src, state.user_def_cursor_mask, Y_CURSOR * 2);
        state.user_screen_mask = true;
        state.user_cursor_mask = true;
        state.hot_x = reg_bx; // XXX limit to -16, 16
        state.hot_y = reg_cx; // XXX limit to -16, 16
        state.cursor_type = 2;
        MOUSEDOS_DrawCursor();
        break;
    }
    case 0x0a: // MS MOUSE v3.0+ - define text cursor
        state.cursor_type  = (reg_bx ? 1 : 0);
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
	    // XXX reg_cx, reg_dx - must be unsigned (high bit 0)
        SetMickeyPixelRate(reg_cx, reg_dx);
        break;
    case 0x10: // MS MOUSE v1.0+ - define screen region for updating
        state.update_region_x[0] = (int16_t)reg_cx;
        state.update_region_y[0] = (int16_t)reg_dx;
        state.update_region_x[1] = (int16_t)reg_si;
        state.update_region_y[1] = (int16_t)reg_di;
        // XXX according to documentation, mouse cursor is hidden in the specified region, and needs to be explicitly turned on again
        MOUSEDOS_DrawCursor();
        break;
    case 0x11: // CuteMouse - get mouse capabilities
        reg_ax           = 0x574d; // Identifier for detection purposes
        reg_bx           = 0;      // Reserved capabilities flags
        reg_cx           = 1;      // Wheel is supported
        state.cute_mouse = true; // This call enables CuteMouse extensions
        state.wheel      = 0;
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
    {
        LOG(LOG_MOUSE, LOG_WARN)("Saving driver state...");
        PhysPt dest = SegPhys(es) + reg_dx;
        MEM_BlockWrite(dest, &state, sizeof(state));
        break;
    }
    case 0x17: // MS MOUSE v6.0+ - load driver state
    {
        LOG(LOG_MOUSE, LOG_WARN)("Loading driver state...");
        PhysPt src = SegPhys(es) + reg_dx; // XXX try to get rid temporary variable
        MEM_BlockRead(src, &state, sizeof(state));
        UpdateDriverActive();
		// FIXME: we should probably fake an event for mouse movement, redraw cursor, etc.
        break;
    }
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
        // XXX implement this
        // XXX 0 = no interrupts, 1 = 30hz, 2 = 50hz, 3=100hz, 4=200hz, above = unpredictable
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
        state.enabled = false; // Just for reporting not doing a thing
                               // with it XXX
        state.oldhidden = state.hidden;
        state.hidden    = 1;
        // XXX  AX = 001F if successful, FFFF if error
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
    case 0x24: // MS MOUSE v6.26+ - get Software version, mouse type, and
               // IRQ number
        reg_bx = 0x805; // version 8.05 woohoo
        reg_ch = 0x04;  // PS/2 type
        reg_cl = 0; // PS/2 mouse; for any other type it would be IRQ
                    // number
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
        reg_cx = (uint16_t)state.max_x;
        reg_dx = (uint16_t)state.max_y;
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
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Load acceleration profiles not implemented");
        break;
    case 0x2c: // MS MOUSE v7.0+ - get acceleration profiles
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Get acceleration profiles not implemented");
        break;
    case 0x2d: // MS MOUSE v7.0+ - select acceleration profile
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Select acceleration profile not implemented");
        break;
    case 0x2e: // MS MOUSE v8.10+ - set acceleration profile names
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Set acceleration profile names not implemented");
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
        reg_ax = (uint16_t)state.min_x;
        reg_bx = (uint16_t)state.min_y;
        reg_cx = (uint16_t)state.max_x;
        reg_dx = (uint16_t)state.max_y;
        break;
    case 0x32: // MS MOUSE v7.05+ - get active advanced functions
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Get active advanced functions not implemented");
        break;
    case 0x33: // MS MOUSE v7.05+ - get switch settings and accelleration
               // profile data
        LOG(LOG_MOUSE, LOG_ERROR)
        ("Get switch settings and acceleration profile data not implemented");
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
    mouse_active.dos_cb_running = false;
    return CBRET_NONE;
}

bool MOUSEDOS_HasCallback(const MouseEventId event_id)
{
    return state.sub_mask & static_cast<uint8_t>(event_id);
}

uintptr_t MOUSEDOS_DoCallback(const MouseEventId event_id,
                              const MouseButtons12S buttons_12S)
{
    mouse_active.dos_cb_running = true;

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
