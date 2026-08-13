// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_TRUETYPE_OUTPUT_H
#define DOSBOX_TRUETYPE_OUTPUT_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "config/config.h"
#include "utils/rgb888.h"

// ***************************************************************************
// Rendering setup
// ***************************************************************************

// To be called within the screen mode setup to determine whether the TTF
// engine should takeover the rendering
bool TRUETYPE_ShouldOverrideScreen();

// To be called just before a frame is sent to the renderer, to determine
// whether we should re-setup the screen mode so that the TTF engine can
// take over or disengage
bool TRUETYPE_ShouldChangeScreenOverride();

// To be called during screen mode setup, when determined that the TTF engine
// wants to override screen rendering, to calculate the screen resolution
void TRUETYPE_CalculateRenderSize(uint32_t& render_width_px, uint32_t& render_height_px);

// Free the rendering cache memory
void TRUETYPE_FreeCacheMemory();

// ***************************************************************************
// Screen drawing
// ***************************************************************************

// To be called at the start of each frame
void TRUETYPE_DrawPrepareScreen();

// To be called at the start of each row of text
void TRUETYPE_DrawPrepareBlockLine(const uint8_t* vram_address, const uint32_t render_line);

// Actual line dwawing
const uint8_t* TRUETYPE_DrawLine(const uint8_t* vram_address,
                                 const uint32_t render_line);
const uint8_t* TRUETYPE_DrawLine(const uint8_t* vram_address,
                                 const uint32_t render_line,
                                 const uint32_t cursor_block,
                                 const Rgb888&  cursor_color);

// ***************************************************************************
// External notifications
// ***************************************************************************

// To be called after the new code page is loaded
void TRUETYPE_NotifyNewCodePage();

// ***************************************************************************
// Configuration
// ***************************************************************************

// Reads the updated configuration parameters
void TRUETYPE_ReadConfigFont(SectionProp& section);
void TRUETYPE_ReadConfigOutput(SectionProp& section);
void TRUETYPE_ReadConfigAspect(SectionProp& section);

// Add the config entries to the given section
void TRUETYPE_AddConfigOptions(SectionProp& section);

// ***************************************************************************
// Information retrieval
// ***************************************************************************

// Returns 'true' if the TTF engine has taken over the screen rendering
bool TRUETYPE_IsOverridingScreen();

// Get the file name of the loaded screen font.
std::string TRUETYPE_GetLoadedScreenFont();

// Tries to shorten the font name to the given length; might be unable to
// shorten it below 10 characters.
std::string TRUETYPE_ShortenFontName(const std::string font_name,
	                             const size_t max_length);

// ***************************************************************************
// Lifecycle
// ***************************************************************************

void TRUETYPE_Init();
void TRUETYPE_Shutdown(); // XXX call it

#endif // DOSBOX_TRUETYPE_OUTPUT_H
