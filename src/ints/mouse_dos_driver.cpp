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

#include "bios.h"
#include "callback.h"
#include "cpu.h"
#include "dos_inc.h"
#include "int10.h"
#include "pic.h"
#include "regs.h"


// This file implements the DOS virtual mouse driver

// Reference:
// - https://www.ctyme.com/intr/int-33.htm

// XXX return to old names
#define DOS_GETPOS_X        (static_cast<Bit16s>(driver_state.x) & driver_state.gran_x)
#define DOS_GETPOS_Y        (static_cast<Bit16s>(driver_state.y) & driver_state.gran_y)
#define DOS_X_CURSOR        16
#define DOS_Y_CURSOR        16
#define DOS_X_MICKEY        8
#define DOS_Y_MICKEY        8
#define DOS_HIGHESTBIT      (1 << (DOS_X_CURSOR - 1))
#define DOS_BUTTONS_NUM     3

static struct { // DOS driver state, can be stored/restored to/from guest memory

    bool     enabled;

    Bit8u    buttons;
    float    x, y;
    Bit16s   wheel;                        // NOTE: only fetch this via 'MouseDOS_GetResetWheel*' calls!

    float    mickey_x, mickey_y;

    Bit16u   times_pressed[DOS_BUTTONS_NUM];
    Bit16u   times_released[DOS_BUTTONS_NUM];
    Bit16u   last_released_x[DOS_BUTTONS_NUM];
    Bit16u   last_released_y[DOS_BUTTONS_NUM];
    Bit16u   last_pressed_x[DOS_BUTTONS_NUM];
    Bit16u   last_pressed_y[DOS_BUTTONS_NUM];
    Bit16u   last_wheel_moved_x;
    Bit16u   last_wheel_moved_y;

    float    mickeysPerPixel_x, mickeysPerPixel_y;
    float    pixelPerMickey_x,  pixelPerMickey_y;

    Bit16s   min_x, max_x, min_y, max_y;
    Bit16s   gran_x, gran_y;

    Bit16s   updateRegion_x[2];
    Bit16s   updateRegion_y[2];

    Bit16u   doubleSpeedThreshold; // FIXME: this should affect mouse movement

    Bit16u   language;
    Bit8u    page;
    Bit8u    mode;

    // sensitivity
    Bit16u   senv_x_val;
    Bit16u   senv_y_val;
    Bit16u   dspeed_val; // FIXME: this should affect mouse movement
    float    senv_x;
    float    senv_y;

    // mouse cursor
    bool     inhibit_draw;
    Bit16u   hidden;
    Bit16u   oldhidden;
    bool     background;
    Bit16s   backposx, backposy;
    Bit8u    backData[DOS_X_CURSOR * DOS_Y_CURSOR];
    Bit16u*  screenMask;
    Bit16u*  cursorMask;
    Bit16s   clipx, clipy;
    Bit16s   hotx, hoty;
    Bit16u   textAndMask, textXorMask;
    Bit16u   cursorType;

    // user callback
    Bit16u   sub_mask;
    Bit16u   sub_seg;
    Bit16u   sub_ofs;

} driver_state;

Bitu     call_int33;
Bitu     call_mouse_bd;
Bitu     call_uir;
RealPt   uir_callback;
bool     in_UIR;

// ***************************************************************************
// Data - cursor/mask
// ***************************************************************************

static constexpr Bit16u DEFAULT_TEXT_AND_MASK = 0x77FF;
static constexpr Bit16u DEFAULT_TEXT_XOR_MASK = 0x7700;

static Bit16u DEFAULT_SCREEN_MASK[DOS_Y_CURSOR] = {
    0x3FFF, 0x1FFF, 0x0FFF, 0x07FF,
    0x03FF, 0x01FF, 0x00FF, 0x007F,
    0x003F, 0x001F, 0x01FF, 0x00FF,
    0x30FF, 0xF87F, 0xF87F, 0xFCFF
};

static Bit16u DEFAULT_CURSOR_MASK[DOS_Y_CURSOR] = {
    0x0000, 0x4000, 0x6000, 0x7000,
    0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7C00, 0x6C00, 0x4600,
    0x0600, 0x0300, 0x0300, 0x0000
};

static Bit16u userdefScreenMask[DOS_Y_CURSOR];
static Bit16u userdefCursorMask[DOS_Y_CURSOR];

// ***************************************************************************
// Text mode cursor
// ***************************************************************************

// Write and read directly to the screen. Do no use int_setcursorpos (LOTUS123)
extern void WriteChar(Bit16u col, Bit16u row, Bit8u page, Bit8u chr, Bit8u attr, bool useattr);
extern void ReadCharAttr(Bit16u col, Bit16u row, Bit8u page, Bit16u * result);

void RestoreCursorBackgroundText() {
    if (driver_state.hidden || driver_state.inhibit_draw) return;

    if (driver_state.background) {
        WriteChar(driver_state.backposx, driver_state.backposy,
                  real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE),
                  driver_state.backData[0], driver_state.backData[1], true);
        driver_state.background = false;
    }
}

void DrawCursorText() {    
    // Restore Background
    RestoreCursorBackgroundText();

    // Check if cursor in update region
    if ((DOS_GETPOS_Y <= driver_state.updateRegion_y[1]) && (DOS_GETPOS_Y >= driver_state.updateRegion_y[0]) &&
        (DOS_GETPOS_X <= driver_state.updateRegion_x[1]) && (DOS_GETPOS_X >= driver_state.updateRegion_x[0])) {
        return;
    }

    // Save Background
    driver_state.backposx = DOS_GETPOS_X >> 3;
    driver_state.backposy = DOS_GETPOS_Y >> 3;
    if (driver_state.mode < 2) driver_state.backposx >>= 1; 

    //use current page (CV program)
    Bit8u page = real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE);
    
    if (driver_state.cursorType == 0) {
        Bit16u result;
        ReadCharAttr(driver_state.backposx, driver_state.backposy, page, &result);
        driver_state.backData[0] = (Bit8u) (result & 0xff);
        driver_state.backData[1] = (Bit8u) (result >> 8);
        driver_state.background  = true;
        // Write Cursor
        result = (result & driver_state.textAndMask) ^ driver_state.textXorMask;
        WriteChar(driver_state.backposx, driver_state.backposy, page, (Bit8u) (result & 0xff), (Bit8u) (result >> 8), true);
    } else {
        Bit16u address=page * real_readw(BIOSMEM_SEG,BIOSMEM_PAGE_SIZE);
        address += (driver_state.backposy * real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS) + driver_state.backposx) * 2;
        address /= 2;
        Bit16u cr = real_readw(BIOSMEM_SEG,BIOSMEM_CRTC_ADDRESS);
        IO_Write(cr    , 0xe);
        IO_Write(cr + 1, (address >> 8) & 0xff);
        IO_Write(cr    , 0xf);
        IO_Write(cr + 1, address & 0xff);
    }
}

// ***************************************************************************
// Graphic mode cursor
// ***************************************************************************

static Bit8u gfxReg3CE[9];
static Bit8u index3C4, gfxReg3C5;

static void SaveVgaRegisters() {
    if (IS_VGA_ARCH) {
        for (Bit8u i=0; i<9; i++) {
            IO_Write    (0x3CE,i);
            gfxReg3CE[i] = IO_Read(0x3CF);
        }
        /* Setup some default values in GFX regs that should work */
        IO_Write(0x3CE,3); IO_Write(0x3Cf,0);                //disable rotate and operation
        IO_Write(0x3CE,5); IO_Write(0x3Cf,gfxReg3CE[5]&0xf0);    //Force read/write mode 0

        //Set Map to all planes. Celtic Tales
        index3C4 = IO_Read(0x3c4);  IO_Write(0x3C4,2);
        gfxReg3C5 = IO_Read(0x3C5); IO_Write(0x3C5,0xF);
    } else if (machine==MCH_EGA) {
        //Set Map to all planes.
        IO_Write(0x3C4,2);
        IO_Write(0x3C5,0xF);
    }
}

static void RestoreVgaRegisters() {
    if (IS_VGA_ARCH) {
        for (Bit8u i=0; i<9; i++) {
            IO_Write(0x3CE,i);
            IO_Write(0x3CF,gfxReg3CE[i]);
        }

        IO_Write(0x3C4,2);
        IO_Write(0x3C5,gfxReg3C5);
        IO_Write(0x3C4,index3C4);
    }
}

static void ClipCursorArea(Bit16s& x1, Bit16s& x2, Bit16s& y1, Bit16s& y2,
                           Bit16u& addx1, Bit16u& addx2, Bit16u& addy) {
    addx1 = addx2 = addy = 0;
    // Clip up
    if (y1 < 0) {
        addy += (-y1);
        y1 = 0;
    }
    // Clip down
    if (y2 > driver_state.clipy) {
        y2 = driver_state.clipy;        
    };
    // Clip left
    if (x1 < 0) {
        addx1 += (-x1);
        x1 = 0;
    };
    // Clip right
    if (x2 > driver_state.clipx) {
        addx2 = x2 - driver_state.clipx;
        x2 = driver_state.clipx;
    };
}

static void RestoreCursorBackground() {
    if (driver_state.hidden || driver_state.inhibit_draw) return;

    SaveVgaRegisters();
    if (driver_state.background) {
        // Restore background
        Bit16s x, y;
        Bit16u addx1, addx2, addy;
        Bit16u dataPos = 0;
        Bit16s x1      = driver_state.backposx;
        Bit16s y1      = driver_state.backposy;
        Bit16s x2      = x1 + DOS_X_CURSOR - 1;
        Bit16s y2      = y1 + DOS_Y_CURSOR - 1;    

        ClipCursorArea(x1, x2, y1, y2, addx1, addx2, addy);

        dataPos = addy * DOS_X_CURSOR;
        for (y = y1; y <= y2; y++) {
            dataPos += addx1;
            for (x = x1; x <= x2; x++) {
                INT10_PutPixel(x, y, driver_state.page, driver_state.backData[dataPos++]);
            };
            dataPos += addx2;
        };
        driver_state.background = false;
    };
    RestoreVgaRegisters();
}

static void DrawCursor() {
    if (driver_state.hidden || driver_state.inhibit_draw) return;
    INT10_SetCurMode();
    // In Textmode ?
    if (CurMode->type == M_TEXT) {
        DrawCursorText();
        return;
    }

    // Check video page. Seems to be ignored for text mode. 
    // hence the text mode handled above this
    // >>> removed because BIOS page is not actual page in some cases, e.g. QQP games
//    if (real_readb(BIOSMEM_SEG,BIOSMEM_CURRENT_PAGE) != driver_state.page) return;

// Check if cursor in update region
/*    if ((DOS_GETPOS_X >= driver_state.updateRegion_x[0]) && (DOS_GETPOS_X <= driver_state.updateRegion_x[1]) &&
        (DOS_GETPOS_Y >= driver_state.updateRegion_y[0]) && (DOS_GETPOS_Y <= driver_state.updateRegion_y[1])) {
        if (CurMode->type==M_TEXT16)
            RestoreCursorBackgroundText();
        else
            RestoreCursorBackground();
        mouse.shown--;
        return;
    }
   */ /*Not sure yet what to do update region should be set to ??? */
         
    // Get Clipping ranges


    driver_state.clipx = (Bit16s) ((Bits) CurMode->swidth  - 1);    // Get from BIOS?
    driver_state.clipy = (Bit16s) ((Bits) CurMode->sheight - 1);

    /* might be vidmode == 0x13?2:1 */
    Bit16s xratio = 640;
    if (CurMode->swidth>0) xratio/=CurMode->swidth;
    if (xratio==0) xratio = 1;
    
    RestoreCursorBackground();

    SaveVgaRegisters();

    // Save Background
    Bit16s x, y;
    Bit16u addx1, addx2, addy;
    Bit16u dataPos   = 0;
    Bit16s x1        = DOS_GETPOS_X / xratio - driver_state.hotx;
    Bit16s y1        = DOS_GETPOS_Y - driver_state.hoty;
    Bit16s x2        = x1 + DOS_X_CURSOR - 1;
    Bit16s y2        = y1 + DOS_Y_CURSOR - 1;    

    ClipCursorArea(x1,x2,y1,y2, addx1, addx2, addy);

    dataPos = addy * DOS_X_CURSOR;
    for (y = y1; y <= y2; y++) {
        dataPos += addx1;
        for (x = x1; x <= x2; x++) {
            INT10_GetPixel(x, y, driver_state.page, &driver_state.backData[dataPos++]);
        };
        dataPos += addx2;
    };
    driver_state.background = true;
    driver_state.backposx   = DOS_GETPOS_X / xratio - driver_state.hotx;
    driver_state.backposy   = DOS_GETPOS_Y - driver_state.hoty;

    // Draw Mousecursor
    dataPos = addy * DOS_X_CURSOR;
    for (y = y1; y <= y2; y++) {
        Bit16u scMask = driver_state.screenMask[addy + y - y1];
        Bit16u cuMask = driver_state.cursorMask[addy + y - y1];
        if (addx1 > 0) { scMask <<= addx1; cuMask <<= addx1; dataPos += addx1; };
        for (x = x1; x <= x2; x++) {
            Bit8u pixel = 0;
            // ScreenMask
            if (scMask & DOS_HIGHESTBIT) pixel = driver_state.backData[dataPos];
            scMask<<=1;
            // CursorMask
            if (cuMask & DOS_HIGHESTBIT) pixel = pixel ^ 0x0f;
            cuMask<<=1;
            // Set Pixel
            INT10_PutPixel(x, y, driver_state.page, pixel);
            dataPos++;
        };
        dataPos += addx2;
    };
    RestoreVgaRegisters();
}

// ***************************************************************************
// DOS driver interface implementation
// ***************************************************************************

static inline Bit8u MouseDOS_GetResetWheel8bit() {
    Bit8s tmp = std::clamp(driver_state.wheel, static_cast<Bit16s>(-0x80), static_cast<Bit16s>(0x7F));
    driver_state.wheel = 0;
    return (tmp >= 0) ? tmp : 0x100 + tmp;
}

static inline Bit16u MouseDOS_GetResetWheel16bit() {
    Bit16s tmp = (driver_state.wheel >= 0) ? driver_state.wheel : 0x10000 + driver_state.wheel;
    driver_state.wheel = 0;
    return tmp;
}

static void MouseDOS_SetMickeyPixelRate(Bit16s px, Bit16s py) {
    if ((px != 0) && (py != 0)) {
        driver_state.mickeysPerPixel_x  = static_cast<float>(px) / DOS_X_MICKEY;
        driver_state.mickeysPerPixel_y  = static_cast<float>(py) / DOS_Y_MICKEY;
        driver_state.pixelPerMickey_x   = DOS_X_MICKEY / static_cast<float>(px);
        driver_state.pixelPerMickey_y   = DOS_Y_MICKEY / static_cast<float>(py);    
    }
}

static void MouseDOS_SetSensitivity(Bit16u px, Bit16u py, Bit16u dspeed) {
    px     = std::min(static_cast<Bit16u>(100), px);
    py     = std::min(static_cast<Bit16u>(100), py);
    dspeed = std::min(static_cast<Bit16u>(100), dspeed);
    // save values
    driver_state.senv_x_val = px;
    driver_state.senv_y_val = py;
    driver_state.dspeed_val = dspeed;
    if ((px != 0) && (py != 0)) {
        px--; // Inspired by CuteMouse 
        py--; // Although their cursor update routine is far more complex then ours
        driver_state.senv_x = (static_cast<float>(px) * px) / 3600.0f + 1.0f / 3.0f;
        driver_state.senv_y = (static_cast<float>(py) * py) / 3600.0f + 1.0f / 3.0f;
     }
}

static void MouseDOS_ResetHardware() {
    PIC_SetIRQMask(12, false); // XXX what does it do?
}

void MouseDOS_BeforeNewVideoMode() {
    if (CurMode->type!=M_TEXT)
        RestoreCursorBackground();
    else
        RestoreCursorBackgroundText();

    driver_state.hidden     = 1;
    driver_state.oldhidden  = 1;
    driver_state.background = false;
}

// FIXME: Does way to much. Many things should be moved to mouse reset one day
void MouseDOS_AfterNewVideoMode(bool setmode) {
    driver_state.inhibit_draw = false;
    // Get the correct resolution from the current video mode
    Bit8u mode = mem_readb(BIOS_VIDEO_MODE);
    if (setmode && mode == driver_state.mode) LOG(LOG_MOUSE,LOG_NORMAL)("New video mode is the same as the old");
    driver_state.gran_x = (Bit16s) 0xffff;
    driver_state.gran_y = (Bit16s) 0xffff;
    switch (mode) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x07: {
        driver_state.gran_x = (mode < 2) ? 0xfff0 : 0xfff8;
        driver_state.gran_y = (Bit16s) 0xfff8;
        Bitu rows = IS_EGAVGA_ARCH ? real_readb(BIOSMEM_SEG,BIOSMEM_NB_ROWS) : 24;
        if ((rows == 0) || (rows > 250)) rows = 25 - 1;
        driver_state.max_y = 8 * (rows + 1) - 1;
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
        if (mode == 0x0d || mode == 0x13) driver_state.gran_x = (Bit16s) 0xfffe;
        driver_state.max_y = 199;
        break;
    case 0x0f:
    case 0x10:
        driver_state.max_y = 349;
        break;
    case 0x11:
    case 0x12:
        driver_state.max_y = 479;
        break;
    default:
        LOG(LOG_MOUSE,LOG_ERROR)("Unhandled videomode %X on reset",mode);
        driver_state.inhibit_draw = true;
        return;
    }

    driver_state.mode                 = mode;
    driver_state.max_x                = 639;
    driver_state.min_x                = 0;
    driver_state.min_y                = 0;
    driver_state.hotx                 = 0;
    driver_state.hoty                 = 0;
    driver_state.screenMask           = DEFAULT_SCREEN_MASK;
    driver_state.cursorMask           = DEFAULT_CURSOR_MASK;
    driver_state.textAndMask          = DEFAULT_TEXT_AND_MASK;
    driver_state.textXorMask          = DEFAULT_TEXT_XOR_MASK;
    driver_state.language             = 0;
    driver_state.page                 = 0;
    driver_state.doubleSpeedThreshold = 64;
    driver_state.updateRegion_y[1]    = -1; // offscreen
    driver_state.cursorType           = 0;
    driver_state.enabled              = true;

    Mouse_ClearQueue();
}

// FIXME: Much too empty, Mouse_NewVideoMode contains stuff that should be in here
static void MouseDOS_Reset()
{
    MouseDOS_BeforeNewVideoMode();
    MouseDOS_AfterNewVideoMode(false);
    MouseDOS_SetMickeyPixelRate(8, 16);

    driver_state.mickey_x   = 0;
    driver_state.mickey_y   = 0;
    driver_state.wheel      = 0;

    driver_state.last_wheel_moved_x = 0;
    driver_state.last_wheel_moved_y = 0;

    for (Bit16u but = 0; but < DOS_BUTTONS_NUM; but++) {
        driver_state.times_pressed[but]   = 0;
        driver_state.times_released[but]  = 0;
        driver_state.last_pressed_x[but]  = 0;
        driver_state.last_pressed_y[but]  = 0;
        driver_state.last_released_x[but] = 0;
        driver_state.last_released_y[but] = 0;
    }

    driver_state.x        = static_cast<float>((driver_state.max_x + 1) / 2);
    driver_state.y        = static_cast<float>((driver_state.max_y + 1) / 2);
    driver_state.sub_mask = 0;
    in_UIR                = false;
}

void MouseDOS_NotifyMoved(Bit32s x_rel, Bit32s y_rel, bool is_captured) {

    float x_rel_sens = x_rel * mouse_config.sensitivity_x;
    float y_rel_sens = y_rel * mouse_config.sensitivity_y;

    float dx = x_rel_sens * driver_state.pixelPerMickey_x;
    float dy = y_rel_sens * driver_state.pixelPerMickey_y;

    if((fabs(x_rel_sens) > 1.0) || (driver_state.senv_x < 1.0)) dx *= driver_state.senv_x;
    if((fabs(y_rel_sens) > 1.0) || (driver_state.senv_y < 1.0)) dy *= driver_state.senv_y;
    // XXX do we need this? if (mouse_bios.useps2callback) dy *= 2;    

    driver_state.mickey_x += (dx * driver_state.mickeysPerPixel_x);
    driver_state.mickey_y += (dy * driver_state.mickeysPerPixel_y);
    if (driver_state.mickey_x >= 32768.0) driver_state.mickey_x -= 65536.0;
    else if (driver_state.mickey_x <= -32769.0) driver_state.mickey_x += 65536.0;
    if (driver_state.mickey_y >= 32768.0) driver_state.mickey_y -= 65536.0;
    else if (driver_state.mickey_y <= -32769.0) driver_state.mickey_y += 65536.0;
    if (is_captured) {
        driver_state.x += dx;
        driver_state.y += dy;
    } else {
        float x = (x_rel - mouse_video.clip_x) / (mouse_video.res_x - 1) * mouse_config.sensitivity_x;
        float y = (y_rel - mouse_video.clip_y) / (mouse_video.res_y - 1) * mouse_config.sensitivity_y;

        if (CurMode->type == M_TEXT) {
            driver_state.x = x * real_readw(BIOSMEM_SEG,BIOSMEM_NB_COLS) * 8;
            driver_state.y = y * (IS_EGAVGA_ARCH ? (real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS) + 1) : 25) * 8;
        } else if ((driver_state.max_x < 2048) || (driver_state.max_y < 2048) || (driver_state.max_x != driver_state.max_y)) {
            if ((driver_state.max_x > 0) && (driver_state.max_y > 0)) {
                driver_state.x = x * driver_state.max_x;
                driver_state.y = y * driver_state.max_y;
            } else {
                driver_state.x += x_rel_sens;
                driver_state.y += y_rel_sens;
            }
        } else { // Games faking relative movement through absolute coordinates. Quite surprising that this actually works..
            driver_state.x += x_rel_sens;
            driver_state.y += y_rel_sens;
        }
    }

    // ignore constraints if using PS2 mouse callback in the BIOS - XXX do we need these?

    // XXX if (!mouse_bios.useps2callback) {
        if (driver_state.x > driver_state.max_x) driver_state.x = driver_state.max_x;
        if (driver_state.x < driver_state.min_x) driver_state.x = driver_state.min_x;
        if (driver_state.y > driver_state.max_y) driver_state.y = driver_state.max_y;
        if (driver_state.y < driver_state.min_y) driver_state.y = driver_state.min_y;
    // XXX } else{
    //    if (driver_state.x >= 32768.0) driver_state.x -= 65536.0;
    //    else if (driver_state.x <= -32769.0) driver_state.x += 65536.0;
    //    if (driver_state.y >= 32768.0) driver_state.y -= 65536.0;
    //    else if (driver_state.y <= -32769.0) driver_state.y += 65536.0;
    // }

    DrawCursor();
}

void MouseDOS_NotifyPressed(Bit8u buttons_12S, Bit8u idx) {
    if (idx >= DOS_BUTTONS_NUM) return;

    driver_state.buttons = buttons_12S;

    driver_state.times_pressed[idx]++;    
    driver_state.last_pressed_x[idx] = DOS_GETPOS_X;
    driver_state.last_pressed_y[idx] = DOS_GETPOS_Y;
}

void MouseDOS_NotifyReleased(Bit8u buttons_12S, Bit8u idx) {
    if (idx >= DOS_BUTTONS_NUM) return;

    driver_state.buttons = buttons_12S;

    driver_state.times_released[idx]++;
    driver_state.last_released_x[idx] = DOS_GETPOS_X;
    driver_state.last_released_y[idx] = DOS_GETPOS_Y;
}

void MouseDOS_NotifyWheel(Bit32s w_rel) {
    driver_state.wheel = std::clamp(w_rel + driver_state.wheel, -0x8000, 0x7fff);
    driver_state.last_wheel_moved_x = DOS_GETPOS_X;
    driver_state.last_wheel_moved_y = DOS_GETPOS_Y;
}

static Bitu INT33_Handler() {
//    LOG(LOG_MOUSE,LOG_NORMAL)("MOUSE: %04X %X %X %d %d",reg_ax,reg_bx,reg_cx,DOS_GETPOS_X,DOS_GETPOS_Y);
    switch (reg_ax) {
    case 0x00: // MS MOUSE - reset driver and read status
        MouseDOS_ResetHardware();
        [[fallthrough]];
    case 0x21: // MS MOUSE v6.0+ - software reset
        reg_ax = 0xffff;
        reg_bx = DOS_BUTTONS_NUM;
        MouseDOS_Reset();
        break;
    case 0x01: // MS MOUSE v1.0+ - show mouse cursor
        if (driver_state.hidden) driver_state.hidden--;
        driver_state.updateRegion_y[1] = -1; //offscreen
        DrawCursor();
        break;
    case 0x02: // MS MOUSE v1.0+ - hide mouse cursor
        {
            if (CurMode->type != M_TEXT) RestoreCursorBackground();
            else RestoreCursorBackgroundText();
            driver_state.hidden++;
        }
        break;
    case 0x03: // MS MOUSE v1.0+ / CuteMouse - return position and button status
        {
            reg_bl = driver_state.buttons;
            reg_bh = MouseDOS_GetResetWheel8bit(); // XXX should it clear the internal counter?
            reg_cx = DOS_GETPOS_X;
            reg_dx = DOS_GETPOS_Y;
        }
        break;
    case 0x04: // MS MOUSE v1.0+ - position mouse cursor
        // If position isn't different from current position, don't change it.
        // (position is rounded so numbers get lost when the rounded number is set)
        // (arena/simulation Wolf)
        if ((Bit16s) reg_cx >= driver_state.max_x) driver_state.x = static_cast<float>(driver_state.max_x);
        else if (driver_state.min_x >= (Bit16s) reg_cx) driver_state.x = static_cast<float>(driver_state.min_x); 
        else if ((Bit16s) reg_cx != DOS_GETPOS_X) driver_state.x = static_cast<float>(reg_cx);

        if ((Bit16s) reg_dx >= driver_state.max_y) driver_state.y = static_cast<float>(driver_state.max_y);
        else if (driver_state.min_y >= (Bit16s) reg_dx) driver_state.y = static_cast<float>(driver_state.min_y); 
        else if ((Bit16s) reg_dx != DOS_GETPOS_Y) driver_state.y = static_cast<float>(reg_dx);
        DrawCursor();
        break;
    case 0x05: // MS MOUSE v1.0+ / CuteMouse - return button press data / mouse wheel data
        {
            Bit16u but = reg_bx;
            if (but == 0xffff){
                reg_bx = MouseDOS_GetResetWheel16bit(); // XXX should it clear the internal counter?
                reg_cx = driver_state.last_wheel_moved_x;
                reg_dx = driver_state.last_wheel_moved_y;
            } else {
                reg_ax = driver_state.buttons;
                if (but >= DOS_BUTTONS_NUM) but = DOS_BUTTONS_NUM - 1;
                reg_cx = driver_state.last_pressed_x[but];
                reg_dx = driver_state.last_pressed_y[but];
                reg_bx = driver_state.times_pressed[but];
                driver_state.times_pressed[but] = 0;
            }
        }
        break;
    case 0x06: // MS MOUSE v1.0+ / CuteMouse - return button release data / mouse wheel data
        {
            Bit16u but = reg_bx;
            if (but == 0xffff){
                reg_bx = MouseDOS_GetResetWheel16bit(); // XXX should it clear the internal counter?
                reg_cx = driver_state.last_wheel_moved_x;
                reg_dx = driver_state.last_wheel_moved_y;
            } else {
                reg_ax = driver_state.buttons;
                if (but >= DOS_BUTTONS_NUM) but = DOS_BUTTONS_NUM - 1;
                reg_cx = driver_state.last_released_x[but];
                reg_dx = driver_state.last_released_y[but];
                reg_bx = driver_state.times_released[but];
                driver_state.times_released[but] = 0;
            }
        }
        break;
    case 0x07: // MS MOUSE v1.0+ - define horizontal cursor range
        {   // Lemmings set 1-640 and wants that. iron seeds set 0-640 but doesn't like 640
            // Iron seed works if newvideo mode with mode 13 sets 0-639
            // Larry 6 actually wants newvideo mode with mode 13 to set it to 0-319
            Bit16s max, min;
            if ((Bit16s) reg_cx < (Bit16s) reg_dx) {
                min = (Bit16s) reg_cx;
                max = (Bit16s) reg_dx;
            } else {
                min = (Bit16s) reg_dx;
                max = (Bit16s) reg_cx;
            }
            driver_state.min_x = min;
            driver_state.max_x = max;
            // Battlechess wants this
            if(driver_state.x > driver_state.max_x) driver_state.x = driver_state.max_x;
            if(driver_state.x < driver_state.min_x) driver_state.x = driver_state.min_x;
            // Or alternatively this: 
            // driver_state.x = (driver_state.max_x - driver_state.min_x + 1) / 2;
            LOG(LOG_MOUSE,LOG_NORMAL)("Define Hortizontal range min:%d max:%d", min, max);
        }
        break;
    case 0x08: // MS MOUSE v1.0+ - define vertical cursor range
        {   // not sure what to take instead of the CurMode (see case 0x07 as well)
            // especially the cases where sheight= 400 and we set it with the mouse_reset to 200
            // disabled it at the moment. Seems to break syndicate who want 400 in mode 13
            Bit16s max, min;
            if ((Bit16s) reg_cx < (Bit16s) reg_dx) {
                min = (Bit16s) reg_cx;
                max = (Bit16s) reg_dx;
            } else {
                min = (Bit16s) reg_dx;
                max = (Bit16s) reg_cx;
            }
            driver_state.min_y = min;
            driver_state.max_y = max;
            // Battlechess wants this
            if(driver_state.y > driver_state.max_y) driver_state.y = driver_state.max_y;
            if(driver_state.y < driver_state.min_y) driver_state.y = driver_state.min_y;
            // Or alternatively this: 
            // driver_state.y = (driver_state.max_y - driver_state.min_y + 1) / 2;
            LOG(LOG_MOUSE,LOG_NORMAL)("Define Vertical range min:%d max:%d", min, max);
        }
        break;
    case 0x09: // MS MOUSE v3.0+ - define GFX cursor
        {
            PhysPt src = SegPhys(es)+reg_dx;
            MEM_BlockRead(src                   , userdefScreenMask, DOS_Y_CURSOR * 2);
            MEM_BlockRead(src + DOS_Y_CURSOR * 2, userdefCursorMask, DOS_Y_CURSOR * 2);
            driver_state.screenMask = userdefScreenMask;
            driver_state.cursorMask = userdefCursorMask;
            driver_state.hotx       = reg_bx;
            driver_state.hoty       = reg_cx;
            driver_state.cursorType = 2;
            DrawCursor();
        }
        break;
    case 0x0a: // MS MOUSE v3.0+ - define text cursor
        driver_state.cursorType  = (reg_bx ? 1 : 0);
        driver_state.textAndMask = reg_cx;
        driver_state.textXorMask = reg_dx;
        if (reg_bx) {
            INT10_SetCursorShape(reg_cl, reg_dl);
            LOG(LOG_MOUSE,LOG_NORMAL)("Hardware Text cursor selected");
        }
        DrawCursor();
        break;
    case 0x27: // MS MOUSE v7.01+ - get screen/cursor masks and mickey counts
        reg_ax = driver_state.textAndMask;
        reg_bx = driver_state.textXorMask;
        [[fallthrough]];
    case 0x0b: // MS MOUSE v1.0+ - read motion data
        reg_cx = static_cast<Bit16s>(driver_state.mickey_x);
        reg_dx = static_cast<Bit16s>(driver_state.mickey_y);
        driver_state.mickey_x = 0;
        driver_state.mickey_y = 0;
        break;
    case 0x0c: // MS MOUSE v1.0+ - define interrupt subroutine parameters
        driver_state.sub_mask = reg_cx & 0xff;
        driver_state.sub_seg  = SegValue(es);
        driver_state.sub_ofs  = reg_dx;
        break;
    case 0x0d: // MS MOUSE v1.0+ - light pen emulation on
    case 0x0e: // MS MOUSE v1.0+ - light pen emulation off
        LOG(LOG_MOUSE,LOG_ERROR)("Mouse light pen emulation not implemented");
        break;
    case 0x0f: // MS MOUSE v1.0+ - define mickey/pixel rate
        MouseDOS_SetMickeyPixelRate(reg_cx, reg_dx);
        break;
    case 0x10: // MS MOUSE v1.0+ - define screen region for updating
        driver_state.updateRegion_x[0] = (Bit16s) reg_cx;
        driver_state.updateRegion_y[0] = (Bit16s) reg_dx;
        driver_state.updateRegion_x[1] = (Bit16s) reg_si;
        driver_state.updateRegion_y[1] = (Bit16s) reg_di;
        DrawCursor();
        break;
    case 0x11: // CuteMouse - get mouse capabilities
        reg_ax = 0x574D; // Identifier for detection purposes
        reg_bx = 0;      // Reserved capabilities flags
        reg_cx = 1;      // Wheel is supported
        // Previous implementation provided Genius Mouse 9.06 function to get
        // number of buttons (https://sourceforge.net/p/dosbox/patches/32/), it was
        // returning 0xffff in reg_ax and number of buttons in reg_bx; I suppose
        // the CuteMouse extensions are more useful
        break;
    case 0x12: // MS MOUSE - set large graphics cursor block
        LOG(LOG_MOUSE,LOG_ERROR)("Large graphics cursor block not implemented");
        break;
    case 0x13: // MS MOUSE v5.0+ - set double-speed threshold
        driver_state.doubleSpeedThreshold = (reg_bx ? reg_bx : 64);
         break;
    case 0x14: // MS MOUSE v3.0+ - exchange event-handler 
        {    
            Bit16u oldSeg  = driver_state.sub_seg;
            Bit16u oldOfs  = driver_state.sub_ofs;
            Bit16u oldMask = driver_state.sub_mask;
            // Set new values
            driver_state.sub_mask = reg_cx;
            driver_state.sub_seg  = SegValue(es);
            driver_state.sub_ofs  = reg_dx;
            // Return old values
            reg_cx = oldMask;
            reg_dx = oldOfs;
            SegSet16(es, oldSeg);
        }
        break;        
    case 0x15: // MS MOUSE v6.0+ - get driver storage space requirements
        reg_bx = sizeof(driver_state);
        break;
    case 0x16: // MS MOUSE v6.0+ - save driver state
        {
            LOG(LOG_MOUSE,LOG_WARN)("Saving driver state...");
            PhysPt dest = SegPhys(es) + reg_dx;
            MEM_BlockWrite(dest, &driver_state, sizeof(driver_state));
        }
        break;
    case 0x17: // MS MOUSE v6.0+ - load driver state
        {
            LOG(LOG_MOUSE,LOG_WARN)("Loading driver state...");
            PhysPt src = SegPhys(es) + reg_dx;
            MEM_BlockRead(src, &driver_state, sizeof(driver_state));
        }
        break;
    case 0x18: // MS MOUSE v6.0+ - set alternate mouse user handler
    case 0x19: // MS MOUSE v6.0+ - set alternate mouse user handler
        LOG(LOG_MOUSE,LOG_WARN)("Alternate mouse user handler not implemented");
        break;
    case 0x1a: // MS MOUSE v6.0+ - set mouse sensitivity
        // FIXME : double mouse speed value
        MouseDOS_SetSensitivity(reg_bx, reg_cx, reg_dx);

        LOG(LOG_MOUSE,LOG_WARN)("Set sensitivity used with %d %d (%d)",reg_bx,reg_cx,reg_dx);
        break;
    case 0x1b: //  MS MOUSE v6.0+ - get mouse sensitivity
        reg_bx = driver_state.senv_x_val;
        reg_cx = driver_state.senv_y_val;
        reg_dx = driver_state.dspeed_val;

        LOG(LOG_MOUSE,LOG_WARN)("Get sensitivity %d %d",reg_bx,reg_cx);
        break;
    case 0x1c: // MS MOUSE v6.0+ - set interrupt rate
        // Can't really set a rate this is host determined
        break;
    case 0x1d: // MS MOUSE v6.0+ - set display page number
        driver_state.page = reg_bl;
        break;
    case 0x1e: // MS MOUSE v6.0+ - get display page number
        reg_bx = driver_state.page;
        break;
    case 0x1f: // MS MOUSE v6.0+ - disable mouse driver
        // ES:BX old mouse driver Zero at the moment TODO
        reg_bx = 0;
        SegSet16(es, 0);       
        driver_state.enabled   = false; // Just for reporting not doing a thing with it
        driver_state.oldhidden = driver_state.hidden;
        driver_state.hidden    = 1;
        break;
    case 0x20: // MS MOUSE v6.0+ - enable mouse driver
        driver_state.enabled   = true;
        driver_state.hidden    = driver_state.oldhidden;
        break;
    case 0x22: // MS MOUSE v6.0+ - set language for messages
        // 00h = English, 01h = French, 02h = Dutch, 03h = German, 04h = Swedish
        // 05h = Finnish, 06h = Spanish, 07h = Portugese, 08h = Italian
        driver_state.language = reg_bx;
        break;
    case 0x23: // MS MOUSE v6.0+ - get language for messages
        reg_bx = driver_state.language;
        break;
    case 0x24: // MS MOUSE v6.26+ - get Software version, mouse type, and IRQ number
        reg_bx = 0x805; // version 8.05 woohoo 
        reg_ch = 0x04;  // PS/2 type
        reg_cl = 0;     // PS/2 mouse; for any other type it would be IRQ number
        break;
    case 0x25: // MS MOUSE v6.26+ - get general driver information
        // TODO: According to PC sourcebook reference
        //       Returns:
        //       AH = status
        //         bit 7 driver type: 1=sys 0=com
        //         bit 6: 0=non-integrated 1=integrated mouse driver
        //         bits 4-5: cursor type  00=software text cursor 01=hardware text cursor 1X=graphics cursor
        //         bits 0-3: Function 28 mouse interrupt rate
        //       AL = Number of MDDS (?)
        //       BX = fCursor lock
        //       CX = FinMouse code
        //       DX = fMouse busy
        LOG(LOG_MOUSE,LOG_ERROR)("General driver information not implemented");
        break;
    case 0x26: // MS MOUSE v6.26+ - get maximum virtual coordinates
        reg_bx = (driver_state.enabled ? 0x0000 : 0xffff);
        reg_cx = (Bit16u) driver_state.max_x;
        reg_dx = (Bit16u) driver_state.max_y;
        break;
    case 0x28: // MS MOUSE v7.0+ - set video mode
        // TODO: According to PC sourcebook
        //       Entry:
        //       CX = Requested video mode
        //       DX = Font size, 0 for default
        //       Returns:
        //       DX = 0 on success, nonzero (requested video mode) if not
        LOG(LOG_MOUSE,LOG_ERROR)("Set video mode not implemented");
        break;
    case 0x29: // MS MOUSE v7.0+ - enumerate video modes
        // TODO: According to PC sourcebook
        //       Entry:
        //       CX = 0 for first, != 0 for next
        //       Exit:
        //       BX:DX = named string far ptr
        //       CX = video mode number
        LOG(LOG_MOUSE,LOG_ERROR)("Enumerate video modes not implemented");
        break;
    case 0x2a: // MS MOUSE v7.01+ - get cursor hot spot
        reg_al = (Bit8u) -driver_state.hidden;    // Microsoft uses a negative byte counter for cursor visibility
        reg_bx = (Bit16u) driver_state.hotx;
        reg_cx = (Bit16u) driver_state.hoty;
        reg_dx = 0x04;    // PS/2 mouse type
        break;
    case 0x2b: // MS MOUSE v7.0+ - load acceleration profiles
        LOG(LOG_MOUSE,LOG_ERROR)("Load acceleration profiles not implemented");
        break;
    case 0x2c: // MS MOUSE v7.0+ - get acceleration profiles
        LOG(LOG_MOUSE,LOG_ERROR)("Get acceleration profiles not implemented");
        break;
    case 0x2d: // MS MOUSE v7.0+ - select acceleration profile
        LOG(LOG_MOUSE,LOG_ERROR)("Select acceleration profile not implemented");
        break;
    case 0x2e: // MS MOUSE v8.10+ - set acceleration profile names
        LOG(LOG_MOUSE,LOG_ERROR)("Set acceleration profile names not implemented");
        break;
    case 0x2f: // MS MOUSE v7.02+ - mouse hardware reset
        LOG(LOG_MOUSE,LOG_ERROR)("INT 33 AX=2F mouse hardware reset not implemented");
        break;
    case 0x30: // MS MOUSE v7.04+ - get/set BallPoint information
        LOG(LOG_MOUSE,LOG_ERROR)("Get/set BallPoint information not implemented");
        break;
    case 0x31: // MS MOUSE v7.05+ - get current minimum/maximum virtual coordinates
        reg_ax = (Bit16u) driver_state.min_x;
        reg_bx = (Bit16u) driver_state.min_y;
        reg_cx = (Bit16u) driver_state.max_x;
        reg_dx = (Bit16u) driver_state.max_y;
        break;
    case 0x32: // MS MOUSE v7.05+ - get active advanced functions
        LOG(LOG_MOUSE,LOG_ERROR)("Get active advanced functions not implemented");
        break;
    case 0x33: // MS MOUSE v7.05+ - get switch settings and accelleration profile data
        LOG(LOG_MOUSE,LOG_ERROR)("Get switch settings and acceleration profile data not implemented");
        break;
    case 0x34: // MS MOUSE v8.0+ - get initialization file
        LOG(LOG_MOUSE,LOG_ERROR)("Get initialization file not implemented");
        break;
    case 0x35: // MS MOUSE v8.10+ - LCD screen large pointer support
        LOG(LOG_MOUSE,LOG_ERROR)("LCD screen large pointer support not implemented");
        break;
    case 0x4d: // MS MOUSE - return pointer to copyright string
        LOG(LOG_MOUSE,LOG_ERROR)("Return pointer to copyright string not implemented");
        break;
    case 0x6d: // MS MOUSE - get version string
        LOG(LOG_MOUSE,LOG_ERROR)("Get version string not implemented");
        break;
    case 0x53C1: // Logitech CyberMan
        LOG(LOG_MOUSE,LOG_NORMAL)("Mouse function 53C1 for Logitech CyberMan called. Ignored by regular mouse driver.");
        break;
    default:
        LOG(LOG_MOUSE,LOG_ERROR)("Mouse Function %04X not implemented!",reg_ax);
        break;
    }
    return CBRET_NONE;
}

static Bitu MOUSE_BD_Handler() {
    // the stack contains offsets to register values
    Bit16u raxpt = real_readw(SegValue(ss), reg_sp + 0x0a);
    Bit16u rbxpt = real_readw(SegValue(ss), reg_sp + 0x08);
    Bit16u rcxpt = real_readw(SegValue(ss), reg_sp + 0x06);
    Bit16u rdxpt = real_readw(SegValue(ss), reg_sp + 0x04);

    // read out the actual values, registers ARE overwritten
    Bit16u rax=real_readw(SegValue(ds), raxpt);
    reg_ax = rax;
    reg_bx = real_readw(SegValue(ds), rbxpt);
    reg_cx = real_readw(SegValue(ds), rcxpt);
    reg_dx = real_readw(SegValue(ds), rdxpt);
//    LOG_MSG("MOUSE BD: %04X %X %X %X %d %d",reg_ax,reg_bx,reg_cx,reg_dx,DOS_GETPOS_X,DOS_GETPOS_Y);
    
    // some functions are treated in a special way (additional registers)
    switch (rax) {
        case 0x09:    // Define GFX Cursor
        case 0x16:    // Save driver state
        case 0x17:    // load driver state
            SegSet16(es, SegValue(ds));
            break;
        case 0x0c:    // Define interrupt subroutine parameters
        case 0x14:    // Exchange event-handler
            if (reg_bx != 0) SegSet16(es, reg_bx);
            else SegSet16(es, SegValue(ds));
            break;
        case 0x10:    // Define screen region for updating
            reg_cx = real_readw(SegValue(ds), rdxpt);
            reg_dx = real_readw(SegValue(ds), rdxpt + 2);
            reg_si = real_readw(SegValue(ds), rdxpt + 4);
            reg_di = real_readw(SegValue(ds), rdxpt + 6);
            break;
        default:
            break;
    }

    INT33_Handler();

    // save back the registers, too
    real_writew(SegValue(ds), raxpt,reg_ax);
    real_writew(SegValue(ds), rbxpt,reg_bx);
    real_writew(SegValue(ds), rcxpt,reg_cx);
    real_writew(SegValue(ds), rdxpt,reg_dx);
    switch (rax) {
        case 0x1f: // Disable Mousedriver
            real_writew(SegValue(ds), rbxpt, SegValue(es));
            break;
        case 0x14: // Exchange event-handler
            real_writew(SegValue(ds), rcxpt, SegValue(es));
            break;
        default:
            break;
    }

    reg_ax = rax;
    return CBRET_NONE;
}

Bitu UIR_Handler() {
    in_UIR = false;
    return CBRET_NONE;
}

bool MouseDOS_HasCallback(Bit8u type) {
    // XXX check whether DOS driver is enablled, see DOSBox-X for reference
    return driver_state.sub_mask & type;
}

bool MouseDOS_CallbackInProgress() {
    // XXX check whether DOS driver is enablled, see DOSBox-X for reference
    return in_UIR;
}

// FIXME: DrawCursor() shall probably be moved inside this handler
Bitu MouseDOS_DoCallback(Bit8u type, Bit8u buttons) {
    in_UIR = true;

    reg_ax = type;
    reg_bl = buttons;
    reg_bh = MouseDOS_GetResetWheel8bit();
    reg_cx = DOS_GETPOS_X;
    reg_dx = DOS_GETPOS_Y;
    reg_si = static_cast<Bit16s>(driver_state.mickey_x);
    reg_di = static_cast<Bit16s>(driver_state.mickey_y);

    CPU_Push16(RealSeg(uir_callback));
    CPU_Push16(RealOff(uir_callback));
    CPU_Push16(driver_state.sub_seg);
    CPU_Push16(driver_state.sub_ofs);

    return CBRET_NONE;
}

void MouseDOS_Init() {
    // Callback for mouse interrupt 0x33
    call_int33 = CALLBACK_Allocate();
    // RealPt i33loc = RealMake(CB_SEG + 1,(call_int33 * CB_SIZE) - 0x10);
    RealPt i33loc = RealMake(DOS_GetMemory(0x1) - 1, 0x10);
    CALLBACK_Setup(call_int33, &INT33_Handler, CB_MOUSE, Real2Phys(i33loc), "Mouse");
    // Wasteland needs low(seg(int33))!=0 and low(ofs(int33))!=0
    real_writed(0, 0x33 << 2, i33loc);

    call_mouse_bd=CALLBACK_Allocate();
    CALLBACK_Setup(call_mouse_bd, &MOUSE_BD_Handler, CB_RETF8, PhysMake(RealSeg(i33loc), RealOff(i33loc) + 2), "MouseBD");
    // pseudocode for CB_MOUSE (including the special backdoor entry point):
    //    jump near i33hd
    //    callback MOUSE_BD_Handler
    //    retf 8
    //  label i33hd:
    //    callback INT33_Handler
    //    iret

    // Callback for mouse user routine return
    call_uir = CALLBACK_Allocate();
    CALLBACK_Setup(call_uir, &UIR_Handler, CB_RETF_CLI, "mouse uir ret");
    uir_callback = CALLBACK_RealPointer(call_uir);

    memset(&driver_state, 0, sizeof(driver_state));
    driver_state.sub_seg = 0x6362; // magic value
    driver_state.hidden  = 1;      // hide cursor on startup
    driver_state.mode    = 0xff;   // non-existing mode

    MouseDOS_ResetHardware();
    MouseDOS_Reset();
    MouseDOS_SetSensitivity(50, 50, 50);
}
