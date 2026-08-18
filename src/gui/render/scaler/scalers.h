// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_RENDER_SCALERS_H
#define DOSBOX_RENDER_SCALERS_H

#include <array>

#include "misc/video.h"

// The additional padding pixels are party for some tweaked text modes (e.g.,
// Q200x25x8 used by Necromancer's DOS Navigator) plus as a safety margin.
//
// This ensures we're not going to crash in the 1600x1200 24-bit (BGR24)
// 0x184 VESA mode when reading a few bytes beyond the end of the buffer (see
// comment above `PixelsPerStep` in `scaler/simple.h`).

// Make sure ScalerMaxWidth remains a multiple of 8
constexpr int ScalerWidthExtraPadding = 8 * 5;

// The maximum supported VESA screen resolution is 1600x1200, but this is not
// enough for TTF output; that would have created a blurry image on modern
// high-end displays, especially with text modes like 132x50.
// 5120×2880 (5K resolution) should be enough for a while; at the moment of
// writing this comment (2026) only a select few high-end monitors can display
// such a resolution.
constexpr int ScalerMaxWidth  = 5120 + ScalerWidthExtraPadding;
constexpr int ScalerMaxHeight = 2880;

extern std::array<int, ScalerMaxHeight> scaler_changed_lines;
extern int scaler_changed_line_index;

typedef void (*ScalerLineHandler)(const void* src);

struct Scaler {
	int x_scale = 0;
	int y_scale = 0;

	ScalerLineHandler line_handlers[6] = {};
};

// Simple scalers
extern Scaler Scale1x;
extern Scaler ScaleHoriz2x;
extern Scaler ScaleVert2x;
extern Scaler Scale2x;

#endif // DOSBOX_RENDER_SCALERS_H
