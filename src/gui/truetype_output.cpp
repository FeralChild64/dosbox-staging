// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "truetype_output.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BBOX_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BITMAP_H

#include "dosbox.h"
#include "dos/dos.h"
#include "hardware/video/vga.h"
#include "misc/logging.h"
#include "utils/math_utils.h"
#include "gui/common.h"
#include "gui/render/render.h"
#include "hardware/pic.h"
#include "ints/int10.h"
#include "misc/cross.h"
#include "misc/std_filesystem.h"
#include "misc/support.h"
#include "misc/unicode.h"
#include "utils/checks.h"
#include "utils/fs_utils.h"
#include "utils/mem_unaligned.h"
#include "utils/restrict.h"

CHECK_NARROWING();

// Default font file and resource directory
static const std::string DefaultFont = "Flexi_IBM_VGA_True.ttf";
static const std::string ResourceDir = "fonts-console";

// #define DEBUG_TTF_NO_CALLIBRATION
// #define DEBUG_TTF_NO_BORDER_SHARPENING
// #define DEBUG_TTF_NO_INVERSE_RENDERING
// #define DEBUG_TTF_NO_ASPECT_CORRECTION

// #define DEBUG_TTF_NO_SCREEN_CACHE

// ***************************************************************************
// Rendering engine contants, tables, and helper functions
// ***************************************************************************

enum class Category
{
	// Character not supported by the current font
	Unsupported,
	// Letter, number, punctuation symbol, etc.
	Regular,
	// A space character
	Space,
	// GUI drawing shape or symbol
	Symbol,
	// GUI drawing shape - up down arrow
	SymbolUpDownArrow,
	// Table or box drawing character
	Drawing,
	// Shaded full blocks
	Shade,
	// Symbols for drawing integrals
	Integral,
	// Ligature to be rendered from two 'Category::Regular' characters
	Ligature,
	// Ligature as above - characters used to render should not overlap
	LigatureNoOverlap,
	// Ukrainian hryvnia currency sign
	Hryvnia,
	// Double tilde sign
	DoubleTilde,

	// TODO: If glyph composition engine is implemented, consider rendering U+F20D
	// (COMMON LATIN CAPITAL LETTER D WITH HOOK AND TAIL) as U+0257 with U+0335
};

// List of code points from 'Box Drawing' and 'Block Elements' Unicode blocks
// which should touch all the borders - to be used for renderer callibration,
// ordered in the order of preferrence
static const std::vector<char32_t> CallibrationCodePointsDrawing = {
	// Typical drawing characters available in code page 437
	0x2588, // FULL BLOCK
	0x253c, // BOX DRAWINGS LIGHT VERTICAL AND HORIZONTAL
	0x256c, // BOX DRAWINGS DOUBLE VERTICAL AND HORIZONTAL
	0x256b, // BOX DRAWINGS VERTICAL DOUBLE AND HORIZONTAL SINGLE
	0x256a, // BOX DRAWINGS VERTICAL SINGLE AND HORIZONTAL DOUBLE
	// Other characters - 'Block Elements'
	0x259a, // QUADRANT UPPER LEFT AND LOWER RIGHT
	0x259e, // QUADRANT UPPER RIGHT AND LOWER LEFT
	0x2599, // QUADRANT UPPER LEFT AND LOWER LEFT AND LOWER RIGHT
	0x259b, // QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER LEFT
	0x259c, // QUADRANT UPPER LEFT AND UPPER RIGHT AND LOWER RIGHT
	0x259f, // QUADRANT UPPER RIGHT AND LOWER LEFT AND LOWER RIGHT
	// Other characters - 'Box Drawing'
	0x253d, // BOX DRAWINGS LEFT HEAVY AND RIGHT VERTICAL LIGHT
	0x253e, // BOX DRAWINGS RIGHT HEAVY AND LEFT VERTICAL LIGHT
	0x253f, // BOX DRAWINGS VERTICAL LIGHT AND HORIZONTAL LIGHT
	0x2540, // BOX DRAWINGS UP HEAVY AND DOWN HORIZONTAL LIGHT
	0x2541, // BOX DRAWINGS DOWN HEAVY AND UP HORIZONTAL LIGHT
	0x2542, // BOX DRAWINGS VERTICAL HEAVY AND HORIZONTAL LIGHT
	0x2543, // BOX DRAWINGS LEFT UP HEAVY AND RIGHT DOWN LIGHT
	0x2544, // BOX DRAWINGS RIGHT UP HEAVY AND LEFT DOWN LIGHT
	0x2545, // BOX DRAWINGS LEFT DOWN HEAVY AND RIGHT UP LIGHT
	0x2546, // BOX DRAWINGS RIGHT DOWN HEAVY AND LEFT UP LIGHT
	0x2547, // BOX DRAWINGS DOWN LIGHT AND UP HORIZONTAL HEAVY
	0x2548, // BOX DRAWINGS UP LIGHT AND DOWN HORIZONTAL HEAVY
	0x2549, // BOX DRAWINGS RIGHT LIGHT AND LEFT VERTICAL HEAVY
	0x254a, // BOX DRAWINGS LEFT LIGHT AND RIGHT VERTICAL HEAVY
	0x254b, // BOX DRAWINGS HEAVY VERTICAL AND HORIZONTAL
	// Last resort characters - 'Box Drawing'
	0x2571, // BOX DRAWINGS LIGHT DIAGONAL UPPER RIGHT TO LOWER LEFT
	0x2572, // BOX DRAWINGS LIGHT DIAGONAL UPPER LEFT TO LOWER RIGHT
	0x2573, // BOX DRAWINGS LIGHT DIAGONAL CROSS
};

static const std::vector<char32_t> CallibrationCodePointsShade = {
	0x2593, // DARK SHADE
	0x2592, // MEDIUM SHADE
	0x2591, // LIGHT SHADE
};

static const std::vector<char32_t> CallibrationCodePointsLigature = {
	0x006d, // LATIN SMALL LETTER M
	0x004d, // LATIN CAPITAL LETTER M
	0x0077, // LATIN SMALL LETTER W
	0x0057, // LATIN CAPITAL LETTER W
	0x00e6, // LATIN SMALL LIGATURE AE
	0x00c6, // LATIN CAPITAL LIGATURE AE
	0x0152, // LATIN CAPITAL LIGATURE OE
	0x0153, // LATIN SMALL LIGATURE OE
};

static const std::vector<char32_t> CallibrationCodePointsAspectRatio = {
	0x25cb, // WHITE CIRCLE
	0x2022, // BULLET
	0x25a0, // BLACK SQUARE
};

// Certain code points needing special treatment
constexpr char32_t CallibrationCodePointIntegralTop    = 0x2321;
constexpr char32_t CallibrationCodePointIntegralBottom = 0x2320;
constexpr char32_t CallibrationCodePointUpDownArrow    = 0x2195;

// All the code points which we can render as a space
static const std::set<char32_t> SpaceCodePoints = {
	0x0000, // NULL
	0x0020, // SPACE
	0x00a0, // NO-BREAK SPACE
	0x1680, // OGHAM SPACE MARK
	0x2000, // EN QUAD
	0x2001, // EM QUAD
	0x2002, // EN SPACE
	0x2003, // EM SPACE
	0x2004, // THREE-PER-EM SPACE
	0x2005, // FOUR-PER-EM SPACE
	0x2006, // SIX-PER-EM SPACE
	0x2007, // FIGURE SPACE
	0x2008, // PUNCTUATION SPACE
	0x2009, // THIN SPACE
	0x200a, // HAIR SPACE
	0x202f, // NARROW NO-BREAK SPACE
	0x205f, // MEDIUM MATHEMATICAL SPACE
	0x3000, // IDEOGRAPGHIC SPACE
};

// Code points which tend to look better (especially when facing other drawing character)
// if they are rendered as inverse of the other code point
static const std::unordered_map<char32_t, char32_t> RenderAsInverse = {
#ifndef DEBUG_TTF_NO_INVERSE_RENDERING
	{ 0x25d8, 0x2022 },
	{ 0x25d9, 0x25cb },
	// Another possible pair: { 0x2593, 0x2591 }
#endif
};

// In some cases, when the font does not contain a glyph for a ligature,
// we can work this around by rendering two letters in a clever way;
// results might vary between fonts, but at least this allows us to
// support code pages containing the given ligature
static const std::unordered_map<char32_t, std::pair<char32_t, char32_t>> SupportedLigatures = {
	// Standard Unicode ligatures
	{ 0x00e6, { 0x0061, 0x0065 } }, // LATIN SMALL LIGATURE AE
	{ 0x0132, { 0x0049, 0x004a } }, // LATIN CAPITAL LIGATURE IJ
	{ 0x0133, { 0x0069, 0x006a } }, // LATIN SMALL LIGATURE IJ
	{ 0x0152, { 0x004f, 0x0045 } }, // LATIN CAPITAL LIGATURE OE
	{ 0x0153, { 0x006e, 0x0065 } }, // LATIN SMALL LIGATURE OE
	{ 0x04a4, { 0x041d, 0x0413 } }, // CYRILLIC CAPITAL LIGATURE EN GHE
	{ 0x04a5, { 0x043d, 0x0433 } }, // CYRILLIC SMALL LIGATURE EN GHE
	{ 0x04d5, { 0x0430, 0x0435 } }, // CYRILLIC SMALL LIGATURE A IE
	// DOSBox private ligatures
	{ 0xedb0, { 0x007a, 0x0142 } }, // PRIVATE DOSBOX PLN SYMBOL
	{ 0xedb2, { 0x0423, 0x041e } }, // PRIVATE DOSBOX CYRILLIC CAPITAL LIGATURE UO
	{ 0xedb3, { 0x0443, 0x043e } }, // PRIVATE DOSBOX CYRILLIC SMALL LIGATURE UO

	// Current ligature fallback support code can only render in a sane way
	// some of the ligatures. It can't work for the following ones:
	// - U+00C6 - LATIN CAPITAL LIGATURE AE
	// - U+04B4 - CYRILLIC CAPITAL LIGATURE TE TSE
	// - U+04B5 - CYRILLIC SMALL LIGATURE TE TSE
	// - U+04D4 - CYRILLIC CAPITAL LIGATURE A IE
	// - U+0587 - ARMENIAN SMALL LIGATURE ECH YIWN
	// - U+05F0 - HEBREW LIGATURE YIDDISH DOUBLE VAV
	// - U+05F1 - HEBREW LIGATURE YIDDISH VAV YOD
	// - U+05F2 - HEBREW LIGATURE YIDDISH DOUBLE YOD
};

// With most ligatures the letters overlap each other a little - there are some
// exceptions, this is the list
static const std::set<char32_t> NoOverlapLigatures = {
	0xedb0 // PRIVATE DOSBOX PLN SYMBOL
};

static Category get_default_category(const char32_t code_point,
                                     const uint8_t dos_code_point = 0)
{
	if (SpaceCodePoints.contains(code_point)) {
		return Category::Space;
	}

#ifdef DEBUG_TTF_NO_POSTPROCESSING
	return Category::Regular;
#endif

	if (dos_code_point < ' ') {

		// U+2195 - UP DOWN ARROW
		// U+21A8 - UP DOWN ARROW WITH BASE
		if (code_point == 0x2195 || code_point == 0x21a8) {
			return Category::SymbolUpDownArrow;
		}

		// U+2022 - BULLET
		if (code_point == 0x2022) {
			return Category::Symbol;
		}

		// 'Arrows' Unicode block
		if (code_point >= 0x2190 && code_point <= 0x21ff) {
			return Category::Symbol;
		}
	}

	// Possible tweak (results might vary, depending on the font) - assign non-GUI
	// symbols to the Category::Regular, these might include:
	// U+2640 - FEMALE SIGN
	// U+2642 - MALE SIGN
	// U+266A - EIGHTH NOTE
	// U+266B - BEAMED EIGHTH NOTES
	// and all the other musical notes, U+2669 - U+266F

	// U+2591 - LIGHT SHADE
	// U+2592 - MEDIUM SHADE
	// U+2593 - DARK SHADE
	if (code_point >= 0x2591 && code_point <= 0x2593) {
		return Category::Shade;
	}

	// U+2320 - TOP HALF INTEGRAL
	// U+2321 - BOTTOM HALF INTEGRAL
	if (code_point == 0x2320 || code_point == 0x2321) {
		return Category::Integral;
	}

	// 'Box Drawing' and 'Block Elements' Unicode blocks
	if (code_point >= 0x2500 && code_point <= 0x259f) {
		return Category::Drawing;
	}

	// 'Geometric Shapes' and 'Miscellaneous Symbols' Unicode blocks
	if (code_point >= 0x25a0 && code_point <= 0x26ff) {
		return Category::Symbol;
	}

	return Category::Regular;
}

// Check if the code point glyph requires aspect ratio correction after
// scaling to look right
static bool needs_aspect_ratio_correction(const char32_t code_point)
{
#ifdef DEBUG_TTF_NO_ASPECT_CORRECTION
	return false;
#endif

	// U+2669 - U+266F - musical notes
	if (code_point >= 0x2669 && code_point <= 0x266f) {
		return false;
	}

	// U+2022 - BULLET
	if (code_point == 0x2022) {
		return true;
	}

	// 'Geometric Shapes' and 'Miscellaneous Symbols' Unicode blocks
	if (code_point >= 0x25a0 && code_point <= 0x26ff) {
		return true;
	}

	return false;
}

// Check if the code point glyph requires sharpening of all the borders to look
// right when placed next to some other glyphs
static bool needs_sharpening_all_borders(const char32_t code_point)
{
#ifdef DEBUG_TTF_NO_BORDER_SHARPENING
	return false;
#endif
	if (get_default_category(code_point) != Category::Drawing) {
		return false;
	}

	if (code_point >= 0x2504 && code_point <= 0x250b) {
		return false;
	}

	if (code_point >= 0x254c && code_point <= 0x254f) {
		return false;
	}

	if (code_point >= 0x2571 && code_point <= 0x2573) {
		return false;
	}

	if (code_point >= 0x2591 && code_point <= 0x2593) {
		return false;
	}

	return true;

}

// Like above, but sharpening should be limited to the top border
static bool needs_sharpening_only_top(const char32_t code_point)
{
#ifdef DEBUG_TTF_NO_BORDER_SHARPENING
	return false;
#endif
	return (code_point == 0x2321);
}

// Like above, but sharpening should be limited to the bottom border
static bool needs_sharpening_only_bottom(const char32_t code_point)
{
#ifdef DEBUG_TTF_NO_BORDER_SHARPENING
	return false;
#endif
	return (code_point == 0x2320);
}

// ***************************************************************************
// Rendering engine single block support implementation
// ***************************************************************************

// Value depending on the output rendering format
constexpr uint8_t BytesPerPixel = 3;

class CharacterBlock
{
public:
	CharacterBlock() = delete;
	CharacterBlock(const uint32_t horizontal_px, const uint32_t vertical_px);

	uint32_t GetWidth() const { return size_horizontal_px; }
	uint32_t GetHeight() const { return size_vertical_px; }

	uint8_t GetPixel(const uint32_t horizontal_px, const uint32_t vertical_px) const;
	void SetPixel(const uint32_t horizontal_px, const uint32_t vertical_px, const uint8_t value);

	void Invert();

	// Render one block, without colors
	void RenderInGrey(uint8_t* const destination, const uint32_t block_line) const;

	// Functions to check if the content touches the border, do not take
	// pixel brightness into account
	bool IsTouchingLeft() const;
	bool IsTouchingRight() const;
	bool IsTouchingTop() const;
	bool IsTouchingBottom() const;

	// Calculates the glyph distance from the border, take the antialiased
	// pixel brightness into account
	float GetDistanceLeft() const;
	float GetDistanceRight() const;
	float GetDistanceTop() const;
	float GetDistanceBottom() const;

	float GetContentWidth() const;
	float GetContentHeight() const;

	// Blend other block with the current one
	void Blend(const CharacterBlock& other);

	// Replace the character with the mirrored image
	void MirrorHorizontally();

	// Functions to remove antialiasing from the given borders, to fix the
	// view when two drawing characters are placed next to each other
	void SharpenAllBorders();
	void SharpenOnlyTop();
	void SharpenOnlyBottom();

private:
	// Helper functions for border sharpening (de-antialiasing)
	uint32_t GetSharpenDepth(const uint32_t depth_check_px) const;
	void SharpenTop(const uint32_t depth_vertical_px);
	void SharpenBottom(const uint32_t depth_vertical_px);
	void SharpenLeft(const uint32_t depth_horizontal_px);
	void SharpenRight(const uint32_t depth_horizontal_px);

	uint32_t size_horizontal_px = 0;
	uint32_t size_vertical_px   = 0;

	std::vector<uint8_t> data = {};
};

CharacterBlock::CharacterBlock(const uint32_t horizontal_px, const uint32_t vertical_px) :
	size_horizontal_px(horizontal_px), size_vertical_px(vertical_px)
{
	assert(horizontal_px < UINT16_MAX);
	assert(vertical_px   < UINT16_MAX);

	data.resize(horizontal_px * vertical_px);
	data.shrink_to_fit();
}

uint8_t CharacterBlock::GetPixel(const uint32_t horizontal_px, const uint32_t vertical_px) const
{
	return data.at(horizontal_px + vertical_px * size_horizontal_px);
}

void CharacterBlock::SetPixel(const uint32_t horizontal_px, const uint32_t vertical_px, const uint8_t value)
{
	data[horizontal_px + vertical_px * size_horizontal_px] = value;
}

void CharacterBlock::Invert()
{
	for (auto &pixel : data) {
		pixel = UINT8_MAX - pixel;
	}
}

void CharacterBlock::RenderInGrey(uint8_t* const destination, const uint32_t block_line) const
{
	for (uint32_t pixel = 0; pixel < size_horizontal_px; ++pixel) {
		const auto value = *(data.begin() + block_line * size_horizontal_px + pixel);

		*(destination + pixel * BytesPerPixel + 0) = value;
		*(destination + pixel * BytesPerPixel + 1) = value;
		*(destination + pixel * BytesPerPixel + 2) = value;

		static_assert(BytesPerPixel >= 3);
		for (uint8_t byte = 3; byte < BytesPerPixel; ++byte) {
			*(destination + pixel * BytesPerPixel + byte) = 0;
		}
	}
}

bool CharacterBlock::IsTouchingLeft() const
{
	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		if (GetPixel(0, y) != 0) {
			return true;
		}
	}

	return false;
}

bool CharacterBlock::IsTouchingRight() const
{
	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		if (GetPixel(size_horizontal_px - 1, y) != 0) {
			return true;
		}
	}

	return false;
}

bool CharacterBlock::IsTouchingTop() const
{
	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		if (GetPixel(x, 0) != 0) {
			return true;
		}
	}

	return false;
}

bool CharacterBlock::IsTouchingBottom() const
{
	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		if (GetPixel(x, size_vertical_px - 1) != 0) {
			return true;
		}
	}

	return false;
}

float CharacterBlock::GetDistanceLeft() const
{
	auto result = static_cast<float>(size_horizontal_px);

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		bool found = false;
		for (uint32_t y = 0; y < size_vertical_px; ++y) {
			const auto value = GetPixel(x, y);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(x);
			distance += (UINT8_MAX - value) / static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetDistanceRight() const
{
	auto result = static_cast<float>(size_horizontal_px);

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		bool found = false;
		for (uint32_t y = 0; y < size_vertical_px; ++y) {
			const auto value = GetPixel(size_horizontal_px - x - 1, y);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(x);
			distance += (UINT8_MAX - value) / static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetDistanceTop() const
{
	auto result = static_cast<float>(size_vertical_px);

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		bool found = false;
		for (uint32_t x = 0; x < size_horizontal_px; ++x) {
			const auto value = GetPixel(x, y);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(y);
			distance += (UINT8_MAX - value) / static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetDistanceBottom() const
{
	auto result = static_cast<float>(size_vertical_px);

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		bool found = false;
		for (uint32_t x = 0; x < size_horizontal_px; ++x) {
			const auto value = GetPixel(x, size_vertical_px - y - 1);
			if (value == 0) {
				continue;
			}

			found = true;

			auto distance = static_cast<float>(y);
			distance += (UINT8_MAX - value) / static_cast<float>(UINT8_MAX);

			result = std::min(result, distance);
		}

		if (found) {
			return result;
		}
	}

	return 0.0f;
}

float CharacterBlock::GetContentWidth() const
{
	auto value = static_cast<float>(size_horizontal_px);
	value -= GetDistanceLeft();
	value -= GetDistanceRight();

	return std::max(0.0f, value);
}

float CharacterBlock::GetContentHeight() const
{
	auto value = static_cast<float>(size_vertical_px);
	value -= GetDistanceTop();
	value -= GetDistanceBottom();

	return std::max(0.0f, value);
}

void CharacterBlock::Blend(const CharacterBlock& other)
{
	const auto limit_horizontal_px = std::min(size_horizontal_px, other.size_horizontal_px);
	const auto limit_vertical_px   = std::min(size_vertical_px, other.size_vertical_px);

	for (uint32_t x = 0; x < limit_horizontal_px; ++x) {
		for (uint32_t y = 0; y < limit_vertical_px; ++y) {
			const auto other_value = other.GetPixel(x, y);
			SetPixel(x, y, std::max(GetPixel(x, y), other_value));
		}
	}
}

void CharacterBlock::MirrorHorizontally()
{
	for (uint32_t x1 = 0; x1 < size_horizontal_px / 2; ++x1) {
		for (uint32_t y = 0; y < size_vertical_px; ++y) {
			const uint32_t x2 = size_horizontal_px - x1 - 1;
			const auto value_1 = GetPixel(x1, y);
			const auto value_2 = GetPixel(x2, y);
			SetPixel(x1, y, value_2);
			SetPixel(x2, y, value_1);
		}
	}
}

uint32_t CharacterBlock::GetSharpenDepth(const uint32_t depth_check_px) const
{
	constexpr uint32_t MinSizeToProcess = 8;
	constexpr float    DepthProportion   = 0.2f;

	static_assert(DepthProportion * MinSizeToProcess >= 1.0f);

	if (depth_check_px < MinSizeToProcess) {
		return  0;
	}

	const auto depth_px = std::lround(DepthProportion * static_cast<float>(depth_check_px));

	constexpr uint32_t Min = 1;
	return std::max(Min, static_cast<uint32_t>(depth_px - 1));
}

void CharacterBlock::SharpenTop(const uint32_t depth_vertical_px)
{
	if (depth_vertical_px == 0) {
		return;
	}

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		const uint32_t y_border = 0;
		const uint32_t y_limit  = y_border + depth_vertical_px;

		auto value = GetPixel(x, y_border);
		for (uint32_t y = y_border + 1; y <= y_limit; ++y) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t y = y_border; y <= y_limit; ++y) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenBottom(const uint32_t depth_vertical_px)
{
	if (depth_vertical_px == 0) {
		return;
	}

	for (uint32_t x = 0; x < size_horizontal_px; ++x) {
		const uint32_t y_border = size_vertical_px - 1;
		const uint32_t y_limit  = y_border - depth_vertical_px;

		auto value = GetPixel(x, y_border);
		for (uint32_t y = y_border - 1; y >= y_limit; --y) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t y = y_border; y >= y_limit; --y) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenLeft(const uint32_t depth_horizontal_px)
{
	if (depth_horizontal_px == 0) {
		return;
	}

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		const uint32_t x_border = 0;
		const uint32_t x_limit  = x_border + depth_horizontal_px;

		auto value = GetPixel(x_border, y);
		for (uint32_t x = x_border + 1; x <= x_limit; ++x) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t x = x_border; x <= x_limit; ++x) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenRight(const uint32_t depth_horizontal_px)
{
	if (depth_horizontal_px == 0) {
		return;
	}

	for (uint32_t y = 0; y < size_vertical_px; ++y) {
		const uint32_t x_border = size_horizontal_px - 1;
		const uint32_t x_limit  = x_border - depth_horizontal_px;

		auto value = GetPixel(x_border, y);
		for (uint32_t x = x_border - 1; x >= x_limit; --x) {
			value = std::max(value, GetPixel(x, y));
		}

		for (uint32_t x = x_border; x >= x_limit; --x) {
			SetPixel(x, y, value);
		}
	}
}

void CharacterBlock::SharpenAllBorders()
{
	const auto depth_horizontal_px = GetSharpenDepth(size_horizontal_px);
	const auto depth_vertical_px   = GetSharpenDepth(size_vertical_px);

	SharpenTop(depth_vertical_px);
	SharpenBottom(depth_vertical_px);
	SharpenLeft(depth_horizontal_px);
	SharpenRight(depth_horizontal_px);
}

void CharacterBlock::SharpenOnlyTop()
{
	SharpenTop(GetSharpenDepth(size_vertical_px));
}

void CharacterBlock::SharpenOnlyBottom()
{
	SharpenBottom(GetSharpenDepth(size_vertical_px));
}

// ***************************************************************************
// Rendering engine font handling
// ***************************************************************************

struct RenderRecipe {
	FT_UInt  glyph_index    = 0;
	Category glyph_category = {};

	std::optional<FT_UInt> glyph_index_secondary = 0;

	bool invert = false;

	bool sharpen_all_borders = false;
	bool sharpen_only_top    = false;
	bool sharpen_only_bottom = false;

	bool needs_aspect_ratio_correction = false;
};

static FT_Library library = {};

class FontWrap {
public:
	bool Load(const std_fs::path &file_path);
	void Unload();

	bool IsLoaded() const { return is_loaded; }
	std_fs::path LoadedFilePath() const {return loaded_file_path; }

	// Check if code page is compatible with the current code page
	bool IsCompatible();

	float GetReportedAspectRatio() const;

	void PreRenderBlocks(const uint32_t width_px, const uint32_t height_px);

	void RenderInGrey(uint8_t* const destination,
	                  const uint8_t character,
	                  const uint32_t line);

	~FontWrap() { Unload(); }

private:
	struct CallibrationData {
		uint32_t block_width_px  = 0;
		uint32_t block_height_px = 0;

		// Shift the rendering by given number of pixels
		float delta_horizontal = 0.0f;
		float delta_vertical   = 0.0f;

		// Stretch to use given number of extra pixels
		float stretch_horizontal = 0.0f;
		float stretch_vertical   = 0.0f;
	};

	struct CallibrationIndexes {
		FT_UInt drawing         = 0;
		FT_UInt shade           = 0;
		FT_UInt integral_top    = 0;
		FT_UInt integral_bottom = 0;
		FT_UInt up_down_arrow   = 0;
		FT_UInt aspect_ratio    = 0;
	};

	RenderRecipe CreateRecipe(const char32_t code_point,
		                  const uint8_t dos_code_point,
		                  std::set<char32_t>& missing_glyphs);

	RenderRecipe CreateRecipeLigature(const char32_t code_point,
                                          std::set<char32_t>& missing_glyphs);

	RenderRecipe CreateRecipeHryvnia(const char32_t code_point,
                                         std::set<char32_t>& missing_glyphs);

	RenderRecipe CreateRecipeDoubleTilde(std::set<char32_t>& missing_glyphs);

	CharacterBlock GetBlock(const FT_Bitmap& bitmap,
                                const FT_Int bitmap_left,
                                const FT_Int bitmap_top,
	                        const uint32_t size_horizontal_px,
	                        const uint32_t size_vertical_px) const;

	CharacterBlock RenderBlockGeneric(const FT_UInt glyph_index,
	                                  const CallibrationData& callibration,
	                                  const FT_Render_Mode render_mode);

	CharacterBlock RenderBlockPreserveAspectRatio(const FT_UInt glyph_index,
	                                              const CallibrationData& callibration,
	                                              const FT_Render_Mode render_mode,
	                                              const bool center = false);

	CharacterBlock RenderBlockSymbol(const FT_UInt glyph_index,
	                                 const bool preserve_aspect,
	                                 const bool is_up_down_arrow);

	CharacterBlock RenderBlockLigature(const FT_UInt glyph_index_1,
	                                   const FT_UInt glyph_index_2,
	                                   const bool should_overlap = true);

	CharacterBlock RenderBlockShade(const FT_UInt glyph_index);

	CharacterBlock RenderBlockHryvnia(const FT_UInt glyph_index_1,
	                                  const FT_UInt glyph_index_2);

	CharacterBlock RenderBlockDoubleTilde(const FT_UInt glyph_index_1,
                                              const FT_UInt glyph_index_2);

	CharacterBlock RenderBlock(const RenderRecipe& recipe);

	FT_BBox GetBoundingBox(const FT_UInt glyph_index);
	FT_BBox GetFontBoundingBox();

	CallibrationData TweakTouchLeft(const FT_UInt glyph_index,
                                        const FontWrap::CallibrationData& base,
                                        const uint8_t max_steps);

	CallibrationData TweakTouchRight(const FT_UInt glyph_index,
                                         const FontWrap::CallibrationData& base,
                                         const uint8_t max_steps);

	CallibrationData TweakTouchTop(const FT_UInt glyph_index,
                                       const FontWrap::CallibrationData& base,
                                       const uint8_t max_steps);

	CallibrationData TweakTouchBottom(const FT_UInt glyph_index,
                                          const FontWrap::CallibrationData& base,
                                          const uint8_t max_steps);

	CallibrationData TweakCenter(const FT_UInt glyph_index,
                                     const FontWrap::CallibrationData& base,
                                     const bool is_up_down_arrow = false);

	CallibrationData TweakGeneric(const Category existing,
	                              const Category fallback,
	                              const FT_UInt glyph_index);

	CallibrationData TweakIntegral(const Category existing,
	                               const Category fallback,
	                               const FT_UInt glyph_index_top,
	                               const FT_UInt glyph_index_bottom);

	void CallibrateRenderer();

	void ReportMissingGlyphs(const std::set<char32_t>& missing_glyphs) const;

	FT_Face face = {};

	static constexpr auto Identity       = 0x10000L;
	static constexpr auto PointsPerPixel = 64;

	static constexpr auto LigatureOverlap = 0.08f;

	// If the face was loaded succesfully
	bool is_loaded = false;

	// Path to the file loaded
	std_fs::path loaded_file_path = {};

	// Values from DOS taken when font compatibility was changed
	// for the last time
	uint16_t       dos_code_page        = 0;
	ScreenFontType dos_screen_font_type = ScreenFontType::Custom;

	// If the font is compatible with the current code page
	bool is_compatible = false;

	static constexpr float   CallibrationStep = 0.5f;
	static constexpr uint8_t MaxStepDelta     = 5;
	static constexpr uint8_t MaxStepStretch   = 5;

	FT_BBox bounding_box = {};

	float bounding_box_width  = 0.0f;
	float bounding_box_height = 0.0f;

	CallibrationIndexes  callibration_indexes          = {};
	std::vector<FT_UInt> callibration_indexes_ligature = {};

	// Glyph indexes relevant to the DOS code page
	std::array<RenderRecipe, UINT8_MAX + 1> recipes = {};

	// Pre-rendered font bitmaps
	uint16_t pre_render_code_page = 0;
	uint32_t pre_render_width_px  = 0;
	uint32_t pre_render_height_px = 0;
	std::vector<CharacterBlock> pre_rendered = {};

	float pixel_aspect_ratio = 1.0f;

	std::unordered_map<Category, CallibrationData> callibration = {};

	// Target distance from the left/right block border when composing a ligature
	// from two glyphs
	float ligature_distance_left  = 0.0f;
	float ligature_distance_right = 0.0f;

	// Distance from the top/bottom block border of the up/down arrow character
	float up_down_arrow_distance_top    = 0.0f;
	float up_down_arrow_distance_bottom = 0.0f;

	// Font aspect ratio detected by analysing certain glyph rendered to the
	// target resolution; to be used for small drawing symbol corrections
	float detected_font_aspect_ratio = 0.0f;

	void ResetCallibration();
	void ApplyCallibration(const CallibrationData& callibration);
};

static FontWrap screen_font = {};

void FontWrap::Unload()
{
	if (is_loaded) {
		FT_Done_Face(face);

		is_loaded        = false;
		loaded_file_path = "";

		dos_code_page        = 0;
		dos_screen_font_type = ScreenFontType::Custom;

		is_compatible = false;

		callibration_indexes = CallibrationIndexes();
		callibration_indexes_ligature.clear();

		ligature_distance_left  = 0.0f;
		ligature_distance_right = 0.0f;

		up_down_arrow_distance_top    = 0.0f;
		up_down_arrow_distance_bottom = 0.0f;

		detected_font_aspect_ratio = 0.0f;

		pre_rendered.clear();
	}
}

bool FontWrap::Load(const std_fs::path& file_path)
{
	Unload();

	const auto result = FT_New_Face(library, file_path.c_str(), 0, &face);
	is_loaded = (result == FT_Err_Ok);
	if (!is_loaded) {
		if (result == FT_Err_Unknown_File_Format) {
			LOG_WARNING("TTF: Unknown font file format, '%s'", file_path.c_str());
		} else {
			LOG_WARNING("TTF: Could not load font file '%s'", file_path.c_str());
		}
		return false;
	}

	if (!(face->face_flags & FT_FACE_FLAG_FIXED_WIDTH)) {
		LOG_WARNING("TTF: Font '%s' is not monospace, it cannot be used", file_path.c_str());
		return false;
	}

	if (face->face_flags & FT_FACE_FLAG_TRICKY) {
		LOG_WARNING("TTF: Font '%s' is considered tricky by FreeType, it cannot be used", file_path.c_str());
		return false;
	}

	if (!(face->face_flags & FT_FACE_FLAG_SCALABLE)) {
		LOG_WARNING("TTF: Font '%s' is a bitmap font, this is not supported", file_path.c_str());
		return false;
	}

	for (const auto code_point : CallibrationCodePointsDrawing) {
		const auto glyph_index = FT_Get_Char_Index(face, code_point);
		if (glyph_index != 0) {
			callibration_indexes.drawing = glyph_index;
			break;
		}
	}

	for (const auto code_point : CallibrationCodePointsShade) {
		const auto glyph_index = FT_Get_Char_Index(face, code_point);
		if (glyph_index != 0) {
			callibration_indexes.shade = glyph_index;
			break;
		}
	}

	for (const auto code_point : CallibrationCodePointsAspectRatio) {
		const auto glyph_index = FT_Get_Char_Index(face, code_point);
		if (glyph_index != 0) {
			callibration_indexes.aspect_ratio = glyph_index;
			break;
		}
	}

	for (const auto code_point : CallibrationCodePointsLigature) {
		const auto glyph_index = FT_Get_Char_Index(face, code_point);
		if (glyph_index != 0) {
			callibration_indexes_ligature.push_back(glyph_index);
		}
	}

	callibration_indexes.integral_top    = FT_Get_Char_Index(face, CallibrationCodePointIntegralTop);
	callibration_indexes.integral_bottom = FT_Get_Char_Index(face, CallibrationCodePointIntegralBottom);
	callibration_indexes.up_down_arrow   = FT_Get_Char_Index(face, CallibrationCodePointUpDownArrow);

	loaded_file_path = file_path;
	return true;
}

RenderRecipe FontWrap::CreateRecipeLigature(const char32_t code_point,
                                            std::set<char32_t>& missing_glyphs)
{
	RenderRecipe recipe = {};
	recipe.glyph_category = Category::Unsupported;

	assert(SupportedLigatures.contains(code_point));

	// We have a fallback code capable of rendering this ligature;
	// check if the font contains the ingredients
	const auto code_point_1 = SupportedLigatures.at(code_point).first;
	const auto code_point_2 = SupportedLigatures.at(code_point).second;

	const auto index_1 = FT_Get_Char_Index(face, code_point_1);
	const auto index_2 = FT_Get_Char_Index(face, code_point_2);

	if (index_1 != 0 && index_2 != 0) {
		recipe.glyph_index           = index_1;
		recipe.glyph_index_secondary = index_2;

		if (NoOverlapLigatures.contains(code_point)) {
			recipe.glyph_category = Category::LigatureNoOverlap;
		} else {
			recipe.glyph_category = Category::Ligature;
		}

		// We can render this ligature with our fallback code
		return recipe;
	}

	// Characters needed by our ligature renderer are not available
	if (is_code_point_private(code_point)) {
		// These are private code points, do not report them;
		// report the missing fallback code points instead
		if (index_1 == 0) {
			missing_glyphs.insert(code_point_1);
		}
		if (index_2 == 0) {
			missing_glyphs.insert(code_point_2);
		}
	} else {
		missing_glyphs.insert(code_point);
	}

	// It's not possible to render this glyph
	return recipe;
}

RenderRecipe FontWrap::CreateRecipeHryvnia(const char32_t code_point,
                                           std::set<char32_t>& missing_glyphs)
{
	RenderRecipe recipe = {};
	recipe.glyph_category = Category::Unsupported;

	// U+0053 - LATIN CAPITAL LETTER S
	constexpr uint32_t code_point_1 = 0x0053;
	// U+002D - HYPHEN-MINUS
	constexpr uint32_t code_point_2 = 0x002d;

	const auto index_1 = FT_Get_Char_Index(face, code_point_1);
	const auto index_2 = FT_Get_Char_Index(face, code_point_2);

	if (index_1 != 0 && index_2 != 0) {
		recipe.glyph_index           = index_1;
		recipe.glyph_index_secondary = index_2;

		recipe.glyph_category = Category::Hryvnia;

		// We can render this glyph with our fallback code
		return recipe;
	}

	// It's not possible to render this glyph
	missing_glyphs.insert(code_point);
	return recipe;
}

RenderRecipe FontWrap::CreateRecipeDoubleTilde(std::set<char32_t>& missing_glyphs)
{
	RenderRecipe recipe = {};
	recipe.glyph_category = Category::Unsupported;

	// U+007E - TILDE
	constexpr uint32_t code_point_1 = 0x007e;
	// U+002D - HYPHEN-MINUS
	constexpr uint32_t code_point_2 = 0x002d;

	const auto index_1 = FT_Get_Char_Index(face, code_point_1);
	// U+002D - HYPHEN-MINUS
	const auto index_2 = FT_Get_Char_Index(face, code_point_2);

	if (index_1 != 0 && index_2 != 0) {
		recipe.glyph_index           = index_1;
		recipe.glyph_index_secondary = index_2;

		recipe.glyph_category = Category::DoubleTilde;

		// We can render this glyph with our fallback code
		return recipe;
	}

	// This is a DOSBox private code point, do not report it;
	// report the missing fallback code point instead
	if (index_1 == 0) {
		missing_glyphs.insert(code_point_1);
	}
	if (index_2 == 0) {
		missing_glyphs.insert(code_point_2);
	}

	// It's not possible to render this glyph
	return recipe;
}

RenderRecipe FontWrap::CreateRecipe(const char32_t code_point,
                                    const uint8_t dos_code_point,
                                    std::set<char32_t>& missing_glyphs)
{
	RenderRecipe recipe = {};

	recipe.glyph_category = get_default_category(code_point, dos_code_point);

	// For code points which should be rendered as space character (i.e.
	// U+00A0 - NO-BREAK SPACE) do not even bother checking the font
	if (recipe.glyph_category == Category::Space) {
		return recipe;
	}

	// Handle normal glyphs - not DOSBox specific, directly provided by the font
	if (!is_code_point_dosbox_specific(code_point)) {
		recipe.glyph_index = FT_Get_Char_Index(face, code_point);

		// Some characters should better be rendered as inverse of others
		if (RenderAsInverse.contains(code_point)) {
			const auto inverse_code_point = RenderAsInverse.at(code_point);
			const auto inverse_index      = FT_Get_Char_Index(face, inverse_code_point);

			if (inverse_index != 0) {
				recipe.glyph_index = inverse_index;
				recipe.invert      = true;
			}
		}

		// Some characters intended for drawing should have non-antialiased
		// borders, as they can touch other drawing elements to, for example,
		// form a longer lines
		if (needs_sharpening_all_borders(code_point)) {
			recipe.sharpen_all_borders = true;
		} else if (needs_sharpening_only_top(code_point)) {
			recipe.sharpen_only_top    = true;
		} else if (needs_sharpening_only_bottom(code_point)) {
			recipe.sharpen_only_bottom = true;
		}

		if (needs_aspect_ratio_correction(code_point)) {
			recipe.needs_aspect_ratio_correction = true;
		}

		// If we have a valid recipe, no further processing is needed
		if (recipe.glyph_index != 0) {
			return recipe;
		}
	}

	// Some ligatures not present in the font (also private code points) can be
	// rendered using our image processing code, by combining two other glyphs
	if (SupportedLigatures.contains(code_point)) {
		return CreateRecipeLigature(code_point, missing_glyphs);
	}

	// We can render the hryvnia sign using letter 'S' and '-' signs
	if (code_point == 0x20b4) {
		return CreateRecipeHryvnia(code_point, missing_glyphs);
	}

	// We can render the double tilde using a normal tilde
	if (code_point == 0xedb1) {
		return CreateRecipeDoubleTilde(missing_glyphs);
	}

	// Glyph is not directly supported by the font
	recipe.glyph_category = Category::Unsupported;
	missing_glyphs.insert(code_point);

	return recipe;
}

void FontWrap::ReportMissingGlyphs(const std::set<char32_t>& missing_glyphs) const
{
	if (missing_glyphs.empty()) {
		return;
	}

	std::set<char32_t> unicode_official_glyphs = {};
	std::set<char32_t> private_known_glyphs    = {};
	std::set<char32_t> private_dosbox_glyphs   = {};
	std::set<char32_t> private_unknown_glyphs  = {};

	// Categorize missing glyphs
	for (const auto code_point : missing_glyphs) {
		if (is_code_point_dosbox_specific(code_point)) {
			private_dosbox_glyphs.insert(code_point);
		} else if (is_code_point_private_well_known(code_point)) {
			private_known_glyphs.insert(code_point);
		} else if (is_code_point_private(code_point)) {
			private_unknown_glyphs.insert(code_point);
			// We should not use any unknown private code points
			assert(false);
		} else {
			unicode_official_glyphs.insert(code_point);
		}
	}

	auto to_string = [&](const std::set<char32_t>& glyphs) {
		std::string message = {};
		std::string line    = {};

		constexpr uint32_t LineBreakAt = 100;

		for (const auto code_point : glyphs) {
			const auto code_point_str = format_str("U+%04X", code_point);
			if (line.size() + 3 + code_point_str.size() > LineBreakAt) {
				if (!message.empty()) {
					message += ",\n";
				}
				message += line;
				line.clear();
			}

			if (!line.empty()) {
				line += ", ";
			}
			line += code_point_str;
		}
		if (!line.empty()) {
			if (!message.empty()) {
				message += ",\n";
			}
			message += line;
		}

		return message;
	};

	LOG_WARNING("TTF: Code page %d cannot be displayed using the current font due to missing glyphs", dos_code_page);

	if (!unicode_official_glyphs.empty()) {
		LOG_WARNING("TTF: Missing Unicode official code points:\n%s",
		            to_string(unicode_official_glyphs).c_str());
	}
	if (!private_known_glyphs.empty()) {
		LOG_WARNING("TTF: Missing Unicode private (well-known) code points:\n%s",
		            to_string(private_known_glyphs).c_str());
	}
	if (!private_dosbox_glyphs.empty()) {
		LOG_WARNING("TTF: Missing Unicode private (DOSBox-specific) code points:\n%s",
		            to_string(private_dosbox_glyphs).c_str());
	}
	if (!private_unknown_glyphs.empty()) {
		LOG_WARNING("TTF: Missing Unicode (private) code points:\n%s",
		            to_string(private_unknown_glyphs).c_str());
	}
}

bool FontWrap::IsCompatible()
{
	if (!is_loaded) {
		return false;
	}

	// Check for the cached result
	if (dos_code_page != 0 &&
	    dos.loaded_codepage  == dos_code_page &&
	    dos.screen_font_type == dos_screen_font_type) {
		return is_compatible;
	}

	// Re-check compatibility
	is_compatible = false;

	// Take a snapshot of important DOS settings
	dos_code_page        = dos.loaded_codepage;
	dos_screen_font_type = dos.screen_font_type;

	// We are only compatible with bundled CPI files or ROM fonts,
	// custom CPI files might contain just about anything and
	// we cannot handle that
	if (dos_screen_font_type != ScreenFontType::Rom &&
	    dos_screen_font_type != ScreenFontType::Bundled) {
		LOG_WARNING("TTF: Only ROM fonts or bundled CPI files can be handled by the font engine");
		return false;
	}

	if (!is_code_page_supported(dos_code_page)) {
		LOG_WARNING("TTF: Code page %d cannot be displayed by the current font engine",
		            dos_code_page);
		return false;
	}

	// Search for the glyph indexes relevant to the DOS code page
	std::set<char32_t> missing_glyphs = {};
	for (size_t idx = 0; idx < recipes.size(); ++idx) {
		const auto code_points = dos_to_unicode(format_str("%c", idx),
		    DosStringConvertMode::ScreenCodesOnly);
		// We can't display characters which use combining marks,
		// FreeType alone can't render such graphemes, we would need a
		// text shaping engine (TODO: write a simple one?).
		char32_t code_point = {};
		if (code_points.empty()) {
			// Code page deos not contain this code point - use space
			code_point = ' ';
		} else if (code_points.size() == 2 && SpaceCodePoints.contains(code_points[0])) {
			// Space + combining mark
			code_point = code_points[1];
		} else if (code_points.size() > 1) {
			LOG_WARNING("TTF: Code page %d uses combining marks, "
			            "this is not supported by the current font engine",
			            dos_code_page);
			return false;
		} else {
			code_point = code_points[0];
		}

		recipes[idx] = CreateRecipe(code_point, static_cast<uint8_t>(idx), missing_glyphs);
	}

	if (missing_glyphs.empty()) {
		is_compatible = true;
		return true;
	}

	// Report the missing glyphs in the log output
	ReportMissingGlyphs(missing_glyphs);
	return false;
}

float FontWrap::GetReportedAspectRatio() const
{
	return static_cast<float>(face->max_advance_width) / static_cast<float>(face->max_advance_height);
}

CharacterBlock FontWrap::GetBlock(const FT_Bitmap& bitmap,
                                  const FT_Int bitmap_left,
                                  const FT_Int bitmap_top,
                                  const uint32_t size_horizontal_px,
                                  const uint32_t size_vertical_px) const
{
	CharacterBlock block(size_horizontal_px, size_vertical_px);

	// Code based on https://freetype.org/freetype2/docs/tutorial/example1.c

	const int x_min = bitmap_left;
	const int y_min = size_vertical_px - bitmap_top;
	const int x_max = x_min + bitmap.width;
	const int y_max = y_min + bitmap.rows;

	auto get_pixel_grey = [&](const int p, const int q) -> uint8_t {
		const auto index = q * bitmap.pitch + p;

		return bitmap.buffer[index];
	};

	auto get_pixel_mono = [&](const int p, const int q) -> uint8_t {
		const auto index = q * bitmap.pitch + p / 8;

		const auto bit_num  = 7 - (p % 8);
		const auto bit_mask = (1 << bit_num);

		return (bitmap.buffer[index] & bit_mask) ? 0xff : 0;
	};

	int p = 0;
	for (int x = x_min; x < x_max; ++x, ++p) {
		if (x < 0 || x >= clamp_to_int32(block.GetWidth())) {
			continue;
		}
		int q = 0;
		for (int y = y_min; y < y_max; ++y, ++q) {
			if (y < 0 || y >= clamp_to_int32(block.GetHeight())) {
				continue;
			}

			uint8_t pixel_value = 0;
			switch (bitmap.pixel_mode) {
			case FT_PIXEL_MODE_GRAY:
				pixel_value = get_pixel_grey(p, q);
				break;
			case FT_PIXEL_MODE_MONO:
				pixel_value = get_pixel_mono(p, q);
				break;
			default:
				assert(false);
				break;
			}

			block.SetPixel(static_cast<uint16_t>(x),
			               static_cast<uint16_t>(y),
			               pixel_value);
		}
	}

	return block;
}

CharacterBlock FontWrap::RenderBlockGeneric(const FT_UInt glyph_index,
                                            const CallibrationData& callibration,
                                            const FT_Render_Mode render_mode)
{
	ApplyCallibration(callibration);

	// XXX check for errors, for every function call

	FT_Load_Glyph(face, glyph_index, 0);
	FT_Render_Glyph(face->glyph, render_mode);

	return GetBlock(face->glyph->bitmap,
	                face->glyph->bitmap_left,
	                face->glyph->bitmap_top,
	                callibration.block_width_px,
	                callibration.block_height_px);
}

CharacterBlock FontWrap::RenderBlockPreserveAspectRatio(const FT_UInt glyph_index,
	                                                const CallibrationData& callibration,
	                                                const FT_Render_Mode render_mode,
	                                                const bool center) // XXX center if needed
{
	const auto block = RenderBlockGeneric(glyph_index, callibration, render_mode);

	// XXX cap the value to prevent going over the block heigth, check if coefficients are sane
        auto callibration_tweaked = callibration;

        const auto coefficient = detected_font_aspect_ratio / pixel_aspect_ratio - 1.0f;
        const auto diff_pixels = coefficient * static_cast<float>(block.GetHeight());

        callibration_tweaked.stretch_vertical += diff_pixels;

        const auto distance_to_middle = callibration_tweaked.delta_vertical +
                                        block.GetDistanceBottom() +
                                        block.GetContentHeight() / 2;

        callibration_tweaked.delta_vertical -= coefficient * distance_to_middle / 2;

        if (center) {
        	// Due to rounding errors and imprecisions the aspect ratio
        	// correction above can sometimes move the previously centered
        	// glyph slightly off-center

        	callibration_tweaked = TweakCenter(glyph_index, callibration_tweaked);
        }

	return RenderBlockGeneric(glyph_index, callibration_tweaked, render_mode);
}

CharacterBlock FontWrap::RenderBlockSymbol(const FT_UInt glyph_index,
                                           const bool preserve_aspect,
                                           const bool is_up_down_arrow)
{
	constexpr auto RenderMode = FT_RENDER_MODE_NORMAL;

	auto callibration_tweaked = TweakCenter(glyph_index, callibration[Category::Symbol], is_up_down_arrow);

	if (preserve_aspect) {
		return RenderBlockPreserveAspectRatio(glyph_index, callibration_tweaked, RenderMode, !is_up_down_arrow);
	} else {
		return RenderBlockGeneric(glyph_index, callibration_tweaked, RenderMode);
	}
}

CharacterBlock FontWrap::RenderBlockLigature(const FT_UInt glyph_index_1,
                                             const FT_UInt glyph_index_2,
                                             const bool should_overlap)
{
	constexpr auto  Flags    = FT_RENDER_MODE_NORMAL;
	constexpr float MinWidth = 1.0f;

	const auto block_width = static_cast<float>(pre_render_width_px);

	const auto overlap = should_overlap ? block_width * LigatureOverlap : 0.0f;

	const auto& callibration_base = callibration[Category::Regular];

	auto callibration_1 = callibration_base;
	auto callibration_2 = callibration_base;

	const auto block_1 = RenderBlockGeneric(glyph_index_1, callibration_base, Flags);
	const auto block_2 = RenderBlockGeneric(glyph_index_2, callibration_base, Flags);

	const auto distance_1_left  = block_1.GetDistanceLeft();
	const auto distance_1_right = block_1.GetDistanceRight();
	const auto distance_2_left  = block_2.GetDistanceLeft();
	const auto distance_2_right = block_2.GetDistanceRight();

	auto width_1 = block_width - distance_1_left - distance_1_right;
	auto width_2 = block_width - distance_2_left - distance_2_right;

	width_1 = std::max(MinWidth, width_1);
	width_2 = std::max(MinWidth, width_2);

	auto width_allowed = block_width - ligature_distance_left - ligature_distance_right;
	width_allowed = std::min(width_allowed, width_1 + width_2);

	auto width_allowed_1 = width_allowed * width_1 / (width_1 + width_2) + overlap;
	auto width_allowed_2 = width_allowed * width_2 / (width_1 + width_2) + overlap;

	width_allowed_1 = std::min(width_allowed_1, width_1);
	width_allowed_2 = std::min(width_allowed_2, width_2);

	callibration_1.stretch_horizontal -= width_1 - width_allowed_1;
	callibration_2.stretch_horizontal -= width_2 - width_allowed_2;

	callibration_1.delta_horizontal -= distance_1_left - ligature_distance_left;
	callibration_2.delta_horizontal -= distance_2_left - ligature_distance_left +
	                                   overlap * 2 - width_allowed_1;

	auto block = RenderBlockGeneric(glyph_index_1, callibration_1, Flags);
	block.Blend(RenderBlockGeneric(glyph_index_2, callibration_2, Flags));

	return block;
}

CharacterBlock FontWrap::RenderBlockShade(const FT_UInt glyph_index)
{
	ApplyCallibration(callibration.at(Category::Shade));

	FT_Load_Glyph(face, glyph_index, 0);
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);

	auto bitmap_left = face->glyph->bitmap_left;
	auto bitmap_top  = face->glyph->bitmap_top;

	const auto glyph_bounding_box = GetBoundingBox(glyph_index);

	const auto glyph_bounding_box_width  = glyph_bounding_box.xMax - glyph_bounding_box.xMin;
	const auto glyph_bounding_box_height = glyph_bounding_box.yMax - glyph_bounding_box.yMin;

	FT_Load_Glyph(face, glyph_index, 0);
	// XXX if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
	// XXX check the number of points/contours against maximum
	// XXX check error codes

	FT_Outline grid = {};

	const auto num_points   = face->glyph->outline.n_points;
	const auto num_contours = face->glyph->outline.n_contours;

	FT_Outline_New(library, num_points * 9, num_contours * 9, &grid);
	grid.flags = face->glyph->outline.flags;

	uint32_t start_point   = 0;
	uint32_t start_contour = 0;
	for (int x = 0; x < 3; x++) {
		for (int y = 0; y < 3; y++) {
			FT_Outline tmp = {};

			FT_Outline_New(library, num_points, num_contours, &tmp);

			FT_Outline_Translate(&tmp,
			                     x * glyph_bounding_box_width,
			                     y * glyph_bounding_box_height);

			for (uint32_t i = 0; i < num_points; ++i) {
				*(grid.points + start_point + i) = *(tmp.points + i);
				*(grid.tags   + start_point + i) = *(tmp.tags + i);
			}
			start_point += num_points;

			for (uint32_t i = 0; i < num_contours; ++i) {
				*(grid.contours + start_contour + i) = *(tmp.contours + i);
			}
			start_contour += num_contours;

			FT_Outline_Done(library, &tmp);
		}
	}

	FT_Bitmap bitmap = {};
	FT_Bitmap_Init(&bitmap);

	FT_Raster_Params params = {};
	params.target = &bitmap;
	params.flags  = FT_RASTER_FLAG_AA;

 	FT_Outline_Render(library, &grid, &params);

 	const auto block = GetBlock(bitmap,
                                    bitmap_left,
                                    bitmap_top,
                                    callibration.at(Category::Shade).block_width_px,
                                    callibration.at(Category::Shade).block_height_px);

	FT_Bitmap_Done(library, &bitmap);
	FT_Outline_Done(library, &grid);

	return block;
}

CharacterBlock FontWrap::RenderBlockHryvnia(const FT_UInt glyph_index_1,
	                                    const FT_UInt glyph_index_2)
{
	constexpr auto Flags = FT_RENDER_MODE_NORMAL;

	const auto block_width  = static_cast<float>(pre_render_width_px);
	const auto block_height = static_cast<float>(pre_render_height_px);

	const auto& callibration_base = callibration[Category::Regular];

	const auto block_1 = RenderBlockGeneric(glyph_index_1, callibration_base, Flags);
	const auto block_2 = RenderBlockGeneric(glyph_index_2, callibration_base, Flags);

	const auto distance_1_left   = block_1.GetDistanceLeft();
	const auto distance_1_right  = block_1.GetDistanceRight();
	const auto distance_1_top    = block_1.GetDistanceTop();
	const auto distance_1_bottom = block_1.GetDistanceBottom();

	const auto distance_2_left   = block_2.GetDistanceLeft();
	const auto distance_2_right  = block_2.GetDistanceRight();
	const auto distance_2_top    = block_2.GetDistanceTop();
	const auto distance_2_bottom = block_2.GetDistanceBottom();

	const auto glyph_index_3 = glyph_index_2;

	auto callibration_1 = callibration_base;
	auto callibration_2 = callibration_base;
	auto callibration_3 = callibration_base;

	// Shift the letter 'S' horizontally so that it is going to end at the right
	// position after being mirrored
	callibration_1.delta_horizontal += distance_1_right - distance_1_left;

	// Position the '-' sign precisely in the middle of the 'S' letter

	const auto width_1  = block_width - distance_1_left - distance_1_right;
	const auto width_2  = block_width - distance_2_left - distance_2_right;
	const auto height_1 = block_height - distance_1_top - distance_1_bottom;
	const auto height_2 = block_height - distance_2_top - distance_2_bottom;

	const auto center_1_horizontal = width_1 / 2 + distance_1_left;
	const auto center_2_horizontal = width_2 / 2 + distance_2_left;
	const auto center_1_vertical   = height_1 / 2 + distance_1_bottom;
	const auto center_2_vertical   = height_2 / 2 + distance_2_bottom;

	callibration_2.delta_horizontal += (center_1_horizontal - center_2_horizontal);
	callibration_2.delta_vertical   += (center_1_vertical - center_2_vertical);

	// Duplicate the '-' sign

	constexpr float DistanceCoefficient = 0.85f;

	callibration_3 = callibration_2;
	callibration_2.delta_vertical += height_2 * DistanceCoefficient;
	callibration_3.delta_vertical -= height_2 * DistanceCoefficient;

	// Compose the final glyph as reversed 'S' blended with two '-' signs
	auto block = RenderBlockGeneric(glyph_index_1, callibration_1, Flags);
	block.MirrorHorizontally();
	block.Blend(RenderBlockGeneric(glyph_index_2, callibration_2, Flags));
	block.Blend(RenderBlockGeneric(glyph_index_3, callibration_3, Flags));

	return block;
}

CharacterBlock FontWrap::RenderBlockDoubleTilde(const FT_UInt glyph_index_1,
                                                const FT_UInt glyph_index_2)
{
	constexpr auto Flags = FT_RENDER_MODE_NORMAL;

	const auto block_height = static_cast<float>(pre_render_height_px);

	const auto& callibration_base = callibration[Category::Regular];

	const auto block_1 = RenderBlockGeneric(glyph_index_1, callibration_base, Flags);
	const auto block_2 = RenderBlockGeneric(glyph_index_2, callibration_base, Flags);

	const auto distance_1_top    = block_1.GetDistanceTop();
	const auto distance_1_bottom = block_1.GetDistanceBottom();
	const auto distance_2_top    = block_2.GetDistanceTop();
	const auto distance_2_bottom = block_2.GetDistanceBottom();

	const auto glyph_index_3 = glyph_index_1;

	auto callibration_1 = callibration_base;
	auto callibration_3 = callibration_base;

	// Position the '~' sign at the height of the '-' sign

	const auto height_1 = block_height - distance_1_top - distance_1_bottom;
	const auto height_2 = block_height - distance_2_top - distance_2_bottom;

	const auto center_1_vertical = height_1 / 2 + distance_1_bottom;
	const auto center_2_vertical = height_2 / 2 + distance_2_bottom;

	callibration_1.delta_vertical += (center_2_vertical - center_1_vertical);

	// Duplicate the '~' sign

	constexpr float DistanceCoefficient = 0.65f;

	callibration_3 = callibration_1;
	callibration_1.delta_vertical += height_1 * DistanceCoefficient;
	callibration_3.delta_vertical -= height_1 * DistanceCoefficient;

	// Compose the final glyph as two '~' signs, one above the other
	auto block = RenderBlockGeneric(glyph_index_1, callibration_1, Flags);
	block.Blend(RenderBlockGeneric(glyph_index_3, callibration_3, Flags));

	return block;
}

CharacterBlock FontWrap::RenderBlock(const RenderRecipe& recipe)
{
	auto block = CharacterBlock(pre_render_width_px, pre_render_height_px);

	const auto category = recipe.glyph_category;

	const bool is_up_down_arrow = (category == Category::SymbolUpDownArrow);
	const bool is_overlapping   = (category == Category::Ligature);
	const bool preserve_aspect  = recipe.needs_aspect_ratio_correction;

	// Call appropriate base renderer
	switch (category) {
	case Category::Space:
		break;
	case Category::Regular:
	case Category::Drawing:
	case Category::Integral:
	case Category::Shade:
		if (preserve_aspect) {
			block = RenderBlockPreserveAspectRatio(recipe.glyph_index,
			                                       callibration.at(category),
                                                               FT_RENDER_MODE_NORMAL);
		} else {
			block = RenderBlockGeneric(recipe.glyph_index,
	                                           callibration.at(category),
	                                           FT_RENDER_MODE_NORMAL);
		}
		break;
	case Category::Symbol:
	case Category::SymbolUpDownArrow:
		block = RenderBlockSymbol(recipe.glyph_index,
		                          preserve_aspect,
		                          is_up_down_arrow);
		break;
	case Category::Ligature:
	case Category::LigatureNoOverlap:
		if (recipe.glyph_index_secondary) {
			block = RenderBlockLigature(recipe.glyph_index,
			                            *recipe.glyph_index_secondary,
			                            is_overlapping);
		} else {
			assert(false);
		}
		break;
	case Category::Hryvnia:
		if (recipe.glyph_index_secondary) {
			block = RenderBlockHryvnia(recipe.glyph_index,
			                           *recipe.glyph_index_secondary);
		} else {
			assert(false);
		}
		break;
	case Category::DoubleTilde:
		block = RenderBlockDoubleTilde(recipe.glyph_index,
                                               *recipe.glyph_index_secondary);
		break;
	default:
		assert(false);
		break;
	}

	// Postprocess the rendered block
	if (recipe.invert) {
		block.Invert();
	} else if (recipe.sharpen_all_borders) {
		block.SharpenAllBorders();
	} else if (recipe.sharpen_only_top) {
		block.SharpenOnlyTop();
	} else if (recipe.sharpen_only_bottom) {
		block.SharpenOnlyBottom();
	}

	return block;
}

FT_BBox FontWrap::GetBoundingBox(const FT_UInt glyph_index)
{
	FT_Load_Glyph(face, glyph_index, 0);
	FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO);

	FT_BBox box    = {};
	FT_Glyph glyph = {};
	FT_Get_Glyph(face->glyph, &glyph);

	// 'FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_UNSCALED, &box);' would be
	// faster, but less precise
	FT_Outline_Get_BBox(&face->glyph->outline, &box);
	FT_Done_Glyph(glyph);

	return box;
}

FT_BBox FontWrap::GetFontBoundingBox()
{
	// XXX this should only be computed when the font is being loaded
	if (callibration_indexes.drawing != 0) {
		return GetBoundingBox(callibration_indexes.drawing);
	}

	auto max = [](const FT_BBox& box1, const FT_BBox& box2) {
		FT_BBox result = {};

		result.xMin = std::min(box1.xMin, box2.xMin);
		result.xMax = std::max(box1.xMax, box2.xMax);
		result.yMin = std::min(box1.yMin, box2.yMin);
		result.yMax = std::max(box1.yMax, box2.yMax);

		return result;
	};

	// We have no good glyph for callibration - so go through all the glyphs
	// in the current code page
	auto box = GetBoundingBox(0);
	for (size_t idx = 0; idx < recipes.size(); ++idx) {
		box = max(box, GetBoundingBox(recipes[idx].glyph_index));
	}

	return box;
}

void FontWrap::ApplyCallibration(const CallibrationData& callibration)
{
	const auto scale_width  = (static_cast<float>(callibration.block_width_px) + callibration.stretch_horizontal) / bounding_box_width;
	const auto scale_height = (static_cast<float>(callibration.block_height_px) + callibration.stretch_vertical) / bounding_box_height;

	FT_Matrix matrix = {};
	FT_Vector delta  = {};

	matrix.xx = std::lround(Identity * scale_width);
	matrix.yy = std::lround(Identity * scale_height);

	delta.x = -bounding_box.xMin + std::lround(callibration.delta_horizontal * PointsPerPixel);
	delta.y = -bounding_box.yMin + std::lround(callibration.delta_vertical   * PointsPerPixel);

	FT_Set_Pixel_Sizes(face, callibration.block_width_px, callibration.block_height_px);
	FT_Set_Transform(face, &matrix, &delta);
}

void FontWrap::ResetCallibration()
{
	FT_Matrix matrix = {};
	FT_Vector delta  = {};
	matrix.xx = Identity;
	matrix.yy = Identity;

	FT_Set_Pixel_Sizes(face, pre_render_width_px, pre_render_height_px);
	FT_Set_Transform(face, &matrix, &delta);
}

FontWrap::CallibrationData FontWrap::TweakTouchLeft(const FT_UInt glyph_index,
                                                    const FontWrap::CallibrationData& base,
                                                    const uint8_t max_steps)
{
	constexpr auto Flags = FT_RENDER_MODE_MONO;

	auto result = base;

	result.delta_horizontal = 0.0f;

	const auto block = RenderBlockGeneric(glyph_index, result, Flags);
	if (block.IsTouchingLeft()) {
		return result;
	}

	auto candidate = result;
	for (uint8_t idx = 1; idx <= max_steps; ++idx) {
		candidate.delta_horizontal = -idx * CallibrationStep;
		if (RenderBlockGeneric(glyph_index, candidate, Flags).IsTouchingLeft()) {
			return candidate;
		}
	}

	return result;
}

FontWrap::CallibrationData FontWrap::TweakTouchRight(const FT_UInt glyph_index,
                                                     const FontWrap::CallibrationData& base,
                                                     const uint8_t max_steps)
{
	constexpr auto Flags = FT_RENDER_MODE_MONO;

	auto result = base;

	result.stretch_horizontal = 0.0f;

	const auto block = RenderBlockGeneric(glyph_index, result, Flags);
	if (block.IsTouchingRight()) {
		return result;
	}

	auto candidate = result;
	for (uint8_t idx = 1; idx <= max_steps; ++idx) {
		candidate.stretch_horizontal = idx * CallibrationStep;
		if (RenderBlockGeneric(glyph_index, candidate, Flags).IsTouchingRight()) {
			return candidate;
		}
	}

	return result;
}

FontWrap::CallibrationData FontWrap::TweakTouchTop(const FT_UInt glyph_index,
                                                   const FontWrap::CallibrationData& base,
                                                   const uint8_t max_steps)
{
	constexpr auto Flags = FT_RENDER_MODE_MONO;

	auto result = base;

	result.stretch_vertical = 0.0f;

	const auto block = RenderBlockGeneric(glyph_index, result, Flags);
	if (block.IsTouchingTop()) {
		return result;
	}

	auto candidate = result;
	for (uint8_t idx = 1; idx <= max_steps; ++idx) {
		candidate.stretch_vertical = idx * CallibrationStep;
		if (RenderBlockGeneric(glyph_index, candidate, Flags).IsTouchingTop()) {
			return candidate;
		}
	}

	return result;
}

FontWrap::CallibrationData FontWrap::TweakTouchBottom(const FT_UInt glyph_index,
                                                      const FontWrap::CallibrationData& base,
                                                      const uint8_t max_steps)
{
	constexpr auto Flags = FT_RENDER_MODE_MONO;

	auto result = base;

	result.stretch_vertical = 0.0f;

	const auto block = RenderBlockGeneric(glyph_index, result, Flags);
	if (block.IsTouchingBottom()) {
		return result;
	}

	auto candidate = result;
	for (uint8_t idx = 1; idx <= max_steps; ++idx) {
		candidate.stretch_vertical = -idx * CallibrationStep;
		if (RenderBlockGeneric(glyph_index, candidate, Flags).IsTouchingBottom()) {
			return candidate;
		}
	}

	return result;
}

FontWrap::CallibrationData FontWrap::TweakCenter(const FT_UInt glyph_index,
                                                 const FontWrap::CallibrationData& base,
                                                 const bool is_up_down_arrow)
{
	constexpr auto RenderMode = FT_RENDER_MODE_NORMAL;

	auto callibration_tweaked = base;

	const auto block = RenderBlockGeneric(glyph_index, callibration_tweaked, RenderMode);

	const auto distance_left   = block.GetDistanceLeft();
	const auto distance_rigth  = block.GetDistanceRight();

	const auto distance_top    = is_up_down_arrow ? up_down_arrow_distance_top : block.GetDistanceTop();
	const auto distance_bottom = is_up_down_arrow ? up_down_arrow_distance_bottom : block.GetDistanceBottom();

	callibration_tweaked.delta_horizontal -= distance_left;
	callibration_tweaked.delta_horizontal += (distance_left + distance_rigth) / 2;

	callibration_tweaked.delta_vertical -= distance_bottom;
	callibration_tweaked.delta_vertical += (distance_top + distance_bottom) / 2;

	return callibration_tweaked;
}

FontWrap::CallibrationData FontWrap::TweakGeneric(const Category existing,
                                                  const Category fallback,
                                                  const FT_UInt glyph_index)
{
	if (glyph_index == 0) {
		return callibration[fallback];
	}

	auto result = callibration[existing];

	result = TweakTouchLeft(glyph_index, result, MaxStepDelta);
	result = TweakTouchBottom(glyph_index, result, MaxStepDelta);

	const auto max_stretch_horizontal = -iroundf(result.delta_horizontal * 2) + MaxStepStretch;
	const auto max_stretch_vertical   = -iroundf(result.delta_vertical   * 2) + MaxStepStretch;

	result = TweakTouchRight(glyph_index, result, clamp_to_uint8(max_stretch_horizontal));
	result = TweakTouchTop(glyph_index, result, clamp_to_uint8(max_stretch_vertical));

	return result;
}

FontWrap::CallibrationData FontWrap::TweakIntegral(const Category existing,
                                                   const Category fallback,
                                                   const FT_UInt glyph_index_top,
                                                   const FT_UInt glyph_index_bottom)
{
	if (glyph_index_top == 0 || glyph_index_bottom == 0) {
		return callibration[fallback];
	}

	auto result = callibration[existing];

	result = TweakTouchBottom(glyph_index_bottom, result, MaxStepDelta);

	const auto max_stretch_y = iroundf(result.delta_vertical * 2) + MaxStepStretch;

	result = TweakTouchTop(glyph_index_top, result, clamp_to_uint8(max_stretch_y));

	return result;
}

void FontWrap::CallibrateRenderer()
{
	ResetCallibration();

	// XXX calculate this once, when loading the font
	bounding_box = GetFontBoundingBox();
	bounding_box_width  = static_cast<float>(bounding_box.xMax - bounding_box.xMin) / PointsPerPixel;
	bounding_box_height = static_cast<float>(bounding_box.yMax - bounding_box.yMin) / PointsPerPixel;
	// XXX assertions, sanity checks

	callibration.clear();
	callibration[Category::Regular].block_width_px  = pre_render_width_px;
	callibration[Category::Regular].block_height_px = pre_render_height_px;

	callibration[Category::Drawing]  = TweakGeneric(Category::Regular,
	                                                Category::Regular,
	                                                callibration_indexes.drawing);

	callibration[Category::Shade]    = TweakGeneric(Category::Regular,
	                                                Category::Drawing,
	                                                callibration_indexes.shade);

	callibration[Category::Integral] = TweakIntegral(Category::Drawing,
	                                                 Category::Drawing,
	                                                 callibration_indexes.integral_top,
	                                                 callibration_indexes.integral_bottom);

	callibration[Category::Symbol] = callibration[Category::Drawing];

	constexpr auto Flags = FT_RENDER_MODE_NORMAL;

	// Additional callibration of ligature rendering
	bool first = true;
	for (const auto glyph_index : callibration_indexes_ligature) {
		const auto block = RenderBlockGeneric(glyph_index,
			                              callibration[Category::Regular],
			                              Flags);

		if (first) {
			ligature_distance_left  = block.GetDistanceLeft();
			ligature_distance_right = block.GetDistanceRight();

			first = false;
			continue;
		}

		ligature_distance_left  = std::min(ligature_distance_left,
		                                   block.GetDistanceLeft());
		ligature_distance_right = std::min(ligature_distance_right,
		                                   block.GetDistanceRight());
	}

	// Additional callibration of up down arrows
	const auto block_arrow = RenderBlockGeneric(callibration_indexes.up_down_arrow,
		                                callibration[Category::Symbol],
		                                Flags);
	up_down_arrow_distance_top    = block_arrow.GetDistanceTop();
	up_down_arrow_distance_bottom = block_arrow.GetDistanceBottom();

	// Detect aspect ratio
	const auto block = RenderBlockGeneric(callibration_indexes.aspect_ratio,
		                              callibration[Category::Symbol],
		                              Flags);

	// XXX check if values are sane
	detected_font_aspect_ratio = block.GetContentWidth() / block.GetContentHeight();
}

void FontWrap::PreRenderBlocks(const uint32_t width_px, const uint32_t height_px)
{
	if (!IsCompatible()) {
		return;
	}

	if (width_px == pre_render_width_px &&
	    height_px == pre_render_height_px &&
	    dos_code_page == pre_render_code_page &&
	    !pre_rendered.empty()) {
	    	// Current pre-render data is still valid
		return;
	}

	// We need to pre-render font for the new size
	pre_rendered.clear();
	pre_render_width_px  = width_px;
	pre_render_height_px = height_px;
	pre_render_code_page = dos_code_page;

	pixel_aspect_ratio = 1.0f; // XXX no longer needed

	CallibrateRenderer();

	pre_rendered.reserve(UINT8_MAX + 1);
	for (uint16_t idx = 0; idx <= UINT8_MAX; ++idx) {
		pre_rendered.push_back(RenderBlock(recipes[idx]));
	}
}

void FontWrap::RenderInGrey(uint8_t* const destination,
			    const uint8_t character,
                            const uint32_t line)
{
	assert(pre_rendered.size() == UINT8_MAX + 1);
	pre_rendered.at(character).RenderInGrey(destination, line % pre_render_height_px);
}

// ***************************************************************************
// Screen related type definitions
// ***************************************************************************

using ColorLookupTable = std::array<Rgb888, UINT8_MAX + 1>;
using RenderDataLine   = std::array<uint8_t, ScalerMaxWidth * BytesPerPixel>; // XXX consider changing this to vector

// ***************************************************************************
// Cache, to skip unnecessary line rendering
// ***************************************************************************

#ifndef DEBUG_TTF_NO_SCREEN_CACHE

struct ScreenCache {

public:
	// Adapt the cache to new screen parameters
	void Configure(const uint32_t new_blocks_horizontal,
                       const uint32_t new_blocks_vertical,
                       const uint32_t new_block_width_px,
                       const uint32_t new_block_height_px);

	// Free all the dynamically allocated memory if TTF subsystem is no longer
	// in use
	void FreeMemory();

	void MarkAllDirty();
	void MarkRenderLineDirty(const uint32_t render_line);
	void MarkBlockLineDirty(const uint32_t block_line);

	bool IsLineDirty(const uint32_t render_line) const;
	void UpdateLine(const uint8_t* vram_address, const uint32_t render_line);

	const RenderDataLine &GetRenderData(const uint32_t render_line);
	const RenderDataLine &GetRenderData(const uint32_t render_line, const uint32_t cursor_block, const Rgb888& cursor_color);

	void UpdateColors(const ColorLookupTable& new_colors_foreground,
	                  const ColorLookupTable& new_colors_background);
	void UpdateVideoMemory(const uint8_t* vram_address,
		               const uint32_t render_line);

private:

	// Screen information
	uint32_t blocks_horizontal = 0;
        uint32_t blocks_vertical   = 0;
        uint32_t block_width_px    = 0;
        uint32_t block_height_px   = 0;
        uint32_t render_width_px   = 0;
        uint32_t render_height_px  = 0;

        // Cached emulation-side screen data
	ColorLookupTable cached_colors_foreground             = {};
	ColorLookupTable cached_colors_background             = {};
	std::vector<std::vector<uint8_t>> cached_video_memory = {};

	// Stored data from behind the cursor   XXX also to CacheEntry
	std::vector<std::vector<uint8_t>> cached_under_cursor = {};

	// Cache and metadata
	using CacheEntry = struct {
		// Rendered data
		RenderDataLine render_data = {};

		std::vector<uint8_t> under_cursor = {};

		bool is_dirty   = true;
		bool has_cursor = false;

		uint32_t cursor_block = {};
		Rgb888   cursor_color = {};
	};
	std::vector<CacheEntry> cache = {};

	void BackupAreaUnderCursor(CacheEntry &cache_entry);
	void RestoreAreaUnderCursor(CacheEntry &cache_entry);
};

static ScreenCache screen_cache;

static void draw_line(RenderDataLine& restrict render_data, const uint8_t *vram_address, const uint32_t render_line);
static void draw_cursor(RenderDataLine& render_data, const uint32_t cursor_block, const Rgb888& cursor_color);

void ScreenCache::Configure(const uint32_t new_blocks_horizontal,
                            const uint32_t new_blocks_vertical,
                            const uint32_t new_block_width_px,
                            const uint32_t new_block_height_px)
{
	if (blocks_horizontal == new_blocks_horizontal &&
	    blocks_vertical   == new_blocks_vertical   &&
	    block_width_px    == new_block_width_px    &&
	    block_height_px   == new_block_height_px) {

		// Same values, keep the existing cache
		return;
	}

	// We have a new screen dimensions
	FreeMemory();

	blocks_horizontal = new_blocks_horizontal;
	blocks_vertical   = new_blocks_vertical;

	block_width_px  = new_block_width_px;
	block_height_px = new_block_height_px;

	render_width_px  = blocks_horizontal * block_width_px;
	render_height_px = blocks_vertical   * block_height_px;

	cached_video_memory.resize(blocks_vertical, std::vector<uint8_t>(blocks_horizontal * 2));

	cache.resize(render_height_px);
	for (auto& cache_entry : cache) {
		cache_entry.under_cursor.resize(block_width_px * BytesPerPixel);
	}
}

void ScreenCache::FreeMemory()
{
	cached_video_memory.clear();

	cache.clear();

	blocks_horizontal = 0;
        blocks_vertical   = 0;
        block_width_px    = 0;
        block_height_px   = 0;

        render_width_px  = 0;
        render_height_px = 0;
}

void ScreenCache::MarkAllDirty()
{
	for (auto& cache_entry : cache) {
		cache_entry.is_dirty = true;
	}
}

void ScreenCache::MarkRenderLineDirty(const uint32_t render_line)
{
	cache[render_line].is_dirty = true;
}

void ScreenCache::MarkBlockLineDirty(const uint32_t block_line)
{
	const uint32_t start = block_height_px * block_line;
	const uint32_t end   = start + block_height_px;

	for (uint32_t idx = start; idx < end; ++idx) {
		cache[idx].is_dirty = true;
	}
}

bool ScreenCache::IsLineDirty(const uint32_t render_line) const
{
	return cache[render_line].is_dirty;
}

void ScreenCache::UpdateLine(const uint8_t* vram_address, const uint32_t render_line)
{
	draw_line(cache[render_line].render_data, vram_address, render_line);

	cache[render_line].has_cursor = false;
	cache[render_line].is_dirty   = false;
}

void ScreenCache::BackupAreaUnderCursor(CacheEntry& cache_entry)
{
	auto source      = cache_entry.render_data.data() + cache_entry.cursor_block * block_width_px * BytesPerPixel;
	auto destination = cache_entry.under_cursor.data();

	std::memcpy(destination, source, block_width_px * BytesPerPixel);
}

void ScreenCache::RestoreAreaUnderCursor(CacheEntry& cache_entry)
{
	if (!cache_entry.has_cursor) {
		return;
	}

	auto source      = cache_entry.under_cursor.data();
	auto destination = cache_entry.render_data.data() + cache_entry.cursor_block * block_width_px * BytesPerPixel;

	std::memcpy(destination, source, block_width_px * BytesPerPixel);

	cache_entry.has_cursor = false;
}

const RenderDataLine &ScreenCache::GetRenderData(const uint32_t render_line)
{
	auto& cache_entry = cache[render_line];
	RestoreAreaUnderCursor(cache_entry);

	return cache_entry.render_data;
}

const RenderDataLine &ScreenCache::GetRenderData(const uint32_t render_line, const uint32_t new_cursor_block, const Rgb888& new_cursor_color)
{
	auto &cache_entry = cache[render_line];

	if (cache_entry.has_cursor &&
	    cache_entry.cursor_block == new_cursor_block &&
	    cache_entry.cursor_color == new_cursor_color) {
		// The cursor is already drawn, color and location matches
		return cache_entry.render_data;
	}

	RestoreAreaUnderCursor(cache_entry);

	cache_entry.cursor_block = new_cursor_block;
	cache_entry.cursor_color = new_cursor_color;

	BackupAreaUnderCursor(cache_entry);
	draw_cursor(cache_entry.render_data, new_cursor_block, new_cursor_color);

	cache_entry.has_cursor = true;

	return cache_entry.render_data;
}

void ScreenCache::UpdateColors(const ColorLookupTable& new_colors_foreground,
                               const ColorLookupTable& new_colors_background)
{
	if (cached_colors_foreground == new_colors_foreground &&
	    cached_colors_background == new_colors_background) {
		return;
	}

	cached_colors_foreground = new_colors_foreground;
	cached_colors_background = new_colors_background;

	MarkAllDirty();
}

void ScreenCache::UpdateVideoMemory(const uint8_t* vram_address, const uint32_t render_line)
{
	const uint32_t block_line = render_line / block_height_px;

	const auto cached_vram = cached_video_memory[block_line].data();
	const auto count_bytes = blocks_horizontal * 2;

	if (0 == std::memcmp(vram_address, cached_vram, count_bytes)) {
		return;
	}

	std::memcpy(cached_vram, vram_address, count_bytes);

	MarkBlockLineDirty(block_line);
}

#endif

// ***************************************************************************
// XXX move these variables and definitions somewhere
// ***************************************************************************


// If the module and a FreeType library is initialized and functional
static bool is_initialized = false;
// If the TTF renderer was enabled in the configuration
static bool is_ttf_enabled = false;
// Last font file read from the configuration
static std::string screen_font_file = {};

// XXX describe this
enum class AspectMode {
	Original,
	Wide,
	Font,
};



static AspectMode aspect_mode = {};


static bool is_ega_vga_font_unaltered = true;










// ***************************************************************************
// Rendering setup
// ***************************************************************************

static bool is_window_ready()
{
	return (GFX_GetWindow() != nullptr);
}

static bool is_supported_text_mode()
{
	// M_CGA_TEXT_COMPOSITE is not supported for now
	return (vga.mode == M_TEXT) || (vga.mode == M_TANDY_TEXT) || (vga.mode == M_HERC_TEXT);
}

bool TRUETYPE_ShouldOverrideScreen()
{
	// XXX handle ReelMagic
	// XXX check if number of character rows/columns is sane
	const bool should_override = is_ttf_enabled && is_initialized && is_window_ready() && is_supported_text_mode() && screen_font.IsCompatible();

	vga.draw.ttf.keep_checking_rom_font = should_override;

	return should_override && is_ega_vga_font_unaltered;
}

bool TRUETYPE_ShouldChangeScreenOverride()
{
	if (!vga.draw.ttf.keep_checking_rom_font) {
		return false;
	}

	if (!is_machine_ega_or_better()) {
		is_ega_vga_font_unaltered = true;
		return false;
	}

	// ROM vs VRAM font compare, for 8x16 and 8x16 fonts
	auto check_font = [&](const uint8_t block_size, const uint8_t *vram_font, const PhysPt rom_font) {
		for (uint16_t block = 0; block < 256; ++block) {
			for (uint8_t line = 0; line < block_size; ++line) {
				const auto rom_value  = phys_readb(rom_font + block * block_size + line);
				const auto vram_value = *(vram_font + block * 32 + line); // XXX constanrt for 32

				if (rom_value != vram_value) {
					return false;
				}
			}
		}
		return true;
	};

	// ROM vs VRAM font compare, for 8x8 font
	auto check_font_8 = [&](const uint8_t *vram_font, const PhysPt rom_font, const bool second_part) {
		const uint8_t offset = second_part ? 128 : 0;
		for (uint16_t block = 0; block < 128; ++block) {
			for (uint8_t line = 0; line < 8; ++line) {
				const auto rom_value  = phys_readb(rom_font + block * 8 + line);
				const auto vram_value = *(vram_font + (block + offset) * 32 + line);

				if (rom_value != vram_value) {
					return false;
				}
			}
		}
		return true;
	};

	const auto vram_font_0 = vga.draw.font_tables[0];
	const auto vram_font_1 = vga.draw.font_tables[0];

	switch (vga.draw.address_line_total) {
		case 16:
		{
			const auto rom_font_16 = RealToPhysical(int10.rom.font_16);

			is_ega_vga_font_unaltered = check_font(16, vram_font_0, rom_font_16) &&
			                            check_font(16, vram_font_1, rom_font_16);
			break;
		}
		case 14:
		{
			const auto rom_font_14 = RealToPhysical(int10.rom.font_14);

			is_ega_vga_font_unaltered = check_font(14, vram_font_0, rom_font_14) &&
			                            check_font(14, vram_font_1, rom_font_14);
			break;
		}
		case 8:
		{
			constexpr bool First_8x8_Part  = false;
			constexpr bool Second_8x8_Part = true;

			const auto rom_font_8_first  = RealToPhysical(int10.rom.font_8_first);
			const auto rom_font_8_second = RealToPhysical(int10.rom.font_8_second);

			is_ega_vga_font_unaltered = check_font_8(vram_font_0, rom_font_8_first, First_8x8_Part) &&
			                            check_font_8(vram_font_1, rom_font_8_first, First_8x8_Part) &&
                                                    check_font_8(vram_font_0, rom_font_8_second, Second_8x8_Part) &&
			                            check_font_8(vram_font_1, rom_font_8_second, Second_8x8_Part);
			break;
		}
		default:
			// Other font height - no standard font available
			is_ega_vga_font_unaltered = false;
			break;
	}

	return (vga.draw.ttf.override && !is_ega_vga_font_unaltered) ||
	       (!vga.draw.ttf.override && is_ega_vga_font_unaltered);
}

void TRUETYPE_CalculateRenderSize(uint32_t &render_width_px, uint32_t &render_height_px)
{
	// XXX 'aspect = stretch' might produce small black borders, to be investigated

	constexpr uint32_t StandardBlocksHorizontal = 80;
	constexpr float    StandardAspectRatio      = 4.0f / 3.0f;

	// Calculate available viewport size (window resolution)
	const auto viewport_px = GFX_GetViewportSizeInPixels();

	const uint32_t window_width_px  = clamp_to_uint32(std::lround(viewport_px.x2() - viewport_px.x1()));
	const uint32_t window_height_px = clamp_to_uint32(std::lround(viewport_px.y2() - viewport_px.y1()));
	// XXX handle 0
	const float window_ratio = static_cast<float>(window_width_px) / static_cast<float>(window_height_px);

	// Get the maximum resolution supported by the scaler
	const uint32_t max_width_px  = ScalerMaxWidth - ScalerWidthExtraPadding;
	const uint32_t max_height_px = ScalerMaxHeight;

	// Calculate number of blocks (characters) in each line.columnt
	const uint32_t blocks_horizontal = vga.draw.ttf.blocks_horizontal;
	const uint32_t blocks_vertical   = vga.draw.ttf.blocks_vertical;

	// Calculate target image ratio
	float target_ratio = StandardAspectRatio;
	if (RENDER_GetAspectRatioCorrectionMode() == AspectRatioCorrectionMode::Stretch) {
		target_ratio = static_cast<float>(window_width_px) / static_cast<float>(window_height_px);
	} else if (aspect_mode == AspectMode::Wide && blocks_horizontal > StandardBlocksHorizontal) {
		target_ratio *= static_cast<float>(blocks_horizontal) / static_cast<float>(StandardBlocksHorizontal);
		// Try not to exceed window ratio
		target_ratio = std::min(target_ratio, window_ratio);
		// Make sure ration is at least the standard 4:3
		target_ratio = std::max(target_ratio, StandardAspectRatio);
	} else if (aspect_mode == AspectMode::Font) {
		// Detect font aspect ratio
		target_ratio = screen_font.GetReportedAspectRatio() * static_cast<float>(blocks_horizontal) / static_cast<float>(blocks_vertical);
	}

	// Initial calculation of the desired image resolution
	uint32_t target_width_px  = std::min(max_width_px, window_width_px);
	uint32_t target_height_px = std::min(max_height_px, window_height_px);

	// Scale dimensions down to fit into max scaler resolution
	const float pre_adjustments_ratio = static_cast<float>(target_width_px) / static_cast<float>(target_height_px);
	if (pre_adjustments_ratio > target_ratio) {
		// We need to shrink the image horizontally
		target_width_px = clamp_to_uint32(std::lround(target_ratio * static_cast<float>(target_height_px)));
	} else if (pre_adjustments_ratio < target_ratio) {
		// We need to shring the image vertically
		target_height_px = clamp_to_uint32(std::lround(static_cast<float>(max_width_px) / target_ratio));
	}

	// Just to be sure the floating point calculations won't result in anything out of range
	target_width_px  = std::min(max_width_px, target_width_px);
	target_height_px = std::min(max_height_px, target_height_px);

	// Calculate block (character) size in pixels
	const uint32_t block_width_px  = target_width_px / blocks_horizontal;
	const uint32_t block_height_px = target_height_px / blocks_vertical;

	// XXX enforce some minimum block size

	// Adapt to the new block size
	screen_font.PreRenderBlocks(block_width_px, block_height_px);

	// Pupulate /return calculation results
	vga.draw.ttf.block_width  = block_width_px;
	vga.draw.ttf.block_height = block_height_px;

	// XXX setup the scaler to upscale the generated image to precisely the desired ratio
	render_width_px  = blocks_horizontal * block_width_px;
	render_height_px = blocks_vertical * block_height_px;

#ifndef DEBUG_TTF_NO_SCREEN_CACHE
	screen_cache.Configure(blocks_horizontal, blocks_vertical, vga.draw.ttf.block_width, vga.draw.ttf.block_height);
#endif
}

void TRUETYPE_FreeCacheMemory()
{
	// XXX call it from vga_draw.cpp when we switch out of TTF mode
#ifndef DEBUG_TTF_NO_SCREEN_CACHE
	screen_cache.FreeMemory();
#endif
}

// ***************************************************************************
// Screen drawing
// ***************************************************************************

// XXX add comments
static ColorLookupTable lookup_colors_foreground = {};
static ColorLookupTable lookup_colors_background = {};

void TRUETYPE_DrawPrepareScreen()
{
	// Prepare color lookup tables to slightly speed up the screen rendering
	for (auto color = 0; color < UINT8_MAX + 1; ++color) {
		lookup_colors_foreground[color] = render.palette.rgb[TXT_FG_Table[color & 0x0f] % 0x100];
		lookup_colors_background[color] = render.palette.rgb[TXT_BG_Table[color >> 4]   % 0x100];
	}

#ifndef DEBUG_TTF_NO_SCREEN_CACHE
	screen_cache.UpdateColors(lookup_colors_foreground, lookup_colors_background);
#endif
}

// XXX add comments
static RenderDataLine row_colors_foreground = {};
static RenderDataLine row_colors_background = {};

static const RenderDataLine EmptyLine = { 0 };

static void prepare_block_color(uint32_t& pixel,
                                RenderDataLine& render_foreground, const uint8_t color_index_foreground,
                                RenderDataLine& render_background, const uint8_t color_index_background)
{
	const auto &color_foreground = lookup_colors_foreground[color_index_foreground];
	const auto &color_background = lookup_colors_background[color_index_background];

	for (uint32_t idx = 0; idx < vga.draw.ttf.block_width; ++idx) {

		render_foreground[pixel] = color_foreground.blue;
		render_background[pixel] = color_background.blue;
		++pixel;

		render_foreground[pixel] = color_foreground.green;
		render_background[pixel] = color_background.green;
		++pixel;

		render_foreground[pixel] = color_foreground.red;
		render_background[pixel] = color_background.red;
		++pixel;

		static_assert(BytesPerPixel >= 3);
		for (uint8_t byte = 3; byte < BytesPerPixel; ++byte) {
			render_foreground[pixel] = 0;
			render_background[pixel] = 0;
			++pixel;
		}
	}
}

static void prepare_block_line(const uint8_t *vram_address)
{
	uint32_t pixel = 0;
	for (uint32_t block = 0; block < vga.draw.blocks; ++block) {
		const auto color = vram_address[block * 2 + 1];

		prepare_block_color(pixel,
		                    row_colors_foreground, color,
		                    row_colors_background, color);
	}
}

static void prepare_block_line_hercules(const uint8_t *vram_address)
{
	const uint8_t ColorIndexBlack = 0x00;
	const uint8_t ColorIndexGrey  = 0x07;
	const uint8_t ColorIndexWhite = 0x0f;

	uint32_t pixel = 0;
	for (uint32_t block = 0; block < vga.draw.blocks; ++block) {
		const auto attributes = vram_address[block * 2 + 1];

		if (!(attributes & 0x77)) {
			// 00h, 80h, 08h, 88h produce black space
           		prepare_block_color(pixel,
		                            row_colors_foreground, ColorIndexBlack,
		                            row_colors_background, ColorIndexBlack);
             		continue;
   		}

		/* XXX port code below

			{
				if (((Bitu)(vga.crtc.underline_location & 0x1f) == line) &&
				    ((attrib & 0x77) == 0x1)) {
					underline = true;
				}
				bg = TXT_BG_Table[0x0];
				if (attrib & 0x8) {
					fg = TXT_FG_Table[0xf];
				} else {
					fg = TXT_FG_Table[0x7];
				}
			}

			uint32_t mask1, mask2;

			if (underline) {
				mask1 = mask2 = FontMask[attrib >> 7];
			} else {
				Bitu font = vga.draw.font_tables[0][chr * 32 + line];

				// blinking
				mask1 = TXT_Font_Table[font >> 4] &
				        FontMask[attrib >> 7];

				mask2 = TXT_Font_Table[font & 0xf] &
				        FontMask[attrib >> 7];
			}

			write_unaligned_uint32(draw, (fg & mask1) | (bg & ~mask1));
			write_unaligned_uint32(draw + 4, (fg & mask2) | (bg & ~mask2));
 */

		// XXX VGA_TEXT_Herc_Draw_Line`
		// - https://www.seasip.info/VintagePC/hercplus.html
		// - https://www.seasip.info/VintagePC/incolor.html

		// XXX this does not work properly - see Norton Commander

		if ((attributes & 0x77) == 0x70) {
			if (attributes & 0x08) {
				prepare_block_color(pixel,
                    			row_colors_foreground, ColorIndexWhite,
                      			row_colors_background, ColorIndexGrey);
			} else {
				// XXX highlighting in NortonCommander - does not work
				prepare_block_color(pixel,
                    			row_colors_foreground, ColorIndexBlack,
                      			row_colors_background, ColorIndexGrey);
			}
		} else if (attributes & 0x08) {
			prepare_block_color(pixel,
                    		row_colors_foreground, ColorIndexWhite,
                      		row_colors_background, ColorIndexBlack);
		} else {
			prepare_block_color(pixel,
                    		row_colors_foreground, ColorIndexGrey,
                      		row_colors_background, ColorIndexBlack);
		}
	}
}

void TRUETYPE_DrawPrepareBlockLine(const uint8_t *vram_address, const uint32_t render_line)
{
	if (render_line % vga.draw.ttf.block_height != 0 ||
	    render_line >= vga.draw.ttf.blocks_vertical * vga.draw.ttf.block_height) {

		// Nothing to do
		return;
	}

	// Prepare BGR color values for SIMD processing;
	// this needs to be done once per row of blocks
	if (vga.mode == M_HERC_TEXT) {
		prepare_block_line_hercules(vram_address);
	} else {
		prepare_block_line(vram_address);
	}

#ifndef DEBUG_TTF_NO_SCREEN_CACHE
	screen_cache.UpdateVideoMemory(vram_address, render_line);
#endif
}

static void draw_line(RenderDataLine& restrict render_data,
	              const uint8_t *vram_address,
                      const uint32_t render_line)
{
	const uint32_t line_in_block = render_line % vga.draw.ttf.block_height;

	// Render the line in BRG format, but in greyscale
	for (uint32_t idx = 0; idx < vga.draw.blocks; ++idx) {
		const auto character = vram_address[idx * 2];

		screen_font.RenderInGrey(render_data.data() + vga.draw.ttf.block_width * idx * BytesPerPixel, character, line_in_block);
	}

	// Now apply foreground/background colors. On high resolution screens
	// this is computationally heavy, but the code was written in such a way
	// that it should be easy for the compiler to vectorize it:
	// - all the input/output collections are arrays (their size is known at
	//   compile time) and the output is marked as restrict to inform the
	//   compiler id does not overlap with the inputs
	// - same array index is used for every value of input and output data;
	//   there are no cross-index dependencies
	// - the loop only uses explicitly specified 8- or 16-bit integers
	// - the loop mainly uses additions, substractions, and multiplications;
	//   the only division is by 0x100, which can be implemented as a bit shift
	// - the loop to be vectorized is marked as such using the OpenMP pragma
	auto colorize = [](const uint8_t color_fg, const uint8_t color_bg, const uint16_t value)
	{
		constexpr uint16_t MaxValue     = 0x100;
		constexpr uint16_t RoundingBias = MaxValue / 2;

		const uint16_t value_fg = value + 1;
		const uint16_t value_bg = MaxValue - value_fg;

		const uint16_t result = clamp_to_uint16(value_fg * color_fg) +
		                        clamp_to_uint16(value_bg * color_bg);

		return clamp_to_uint8((result + RoundingBias) / MaxValue);
	};

	#pragma omp simd
	for (uint32_t idx = 0; idx < vga.draw.blocks * vga.draw.ttf.block_width * BytesPerPixel; ++idx) {
		render_data[idx] = colorize(row_colors_foreground[idx],
		                            row_colors_background[idx],
		                            render_data[idx]);
	}
}

static void draw_cursor(RenderDataLine& render_data, const uint32_t cursor_block, const Rgb888& cursor_color)
{
	auto position = render_data.data() + cursor_block * vga.draw.ttf.block_width * BytesPerPixel;

	for (uint32_t idx = 0; idx < vga.draw.ttf.block_width; ++idx) {
		*(position++) = cursor_color.blue;
		*(position++) = cursor_color.green;
		*(position++) = cursor_color.red;
	}
}

const uint8_t* TRUETYPE_DrawLine(const uint8_t* vram_address,
                                 const uint32_t render_line)
{
	if (render_line >= vga.draw.ttf.blocks_vertical * vga.draw.ttf.block_height) {
		return EmptyLine.data();
	}

#ifndef DEBUG_TTF_NO_SCREEN_CACHE

	if (screen_cache.IsLineDirty(render_line)) {
		screen_cache.UpdateLine(vram_address, render_line);
	}

	return screen_cache.GetRenderData(render_line).data();

#else
	static RenderDataLine render_data = {};

	draw_line(render_data, vram_address, render_line);

	return render_data.data();
#endif
}

const uint8_t* TRUETYPE_DrawLine(const uint8_t* vram_address,
                                 const uint32_t render_line,
                                 const uint32_t cursor_block,
                                 const Rgb888&  cursor_color)
{
#ifndef DEBUG_TTF_NO_SCREEN_CACHE

	if (screen_cache.IsLineDirty(render_line)) {
		screen_cache.UpdateLine(vram_address, render_line);
	}

	return screen_cache.GetRenderData(render_line, cursor_block, cursor_color).data();

#else
	static RenderDataLine render_data = {};

	draw_line(render_data, vram_address, render_line);
	draw_cursor(render_data, cursor_block, cursor_color);

	return render_data.data();
#endif
}

// ***************************************************************************
// External notifications
// ***************************************************************************

void TRUETYPE_NotifyNewCodePage()
{
	const auto should_override_screen= TRUETYPE_ShouldOverrideScreen();

	if (should_override_screen && vga.draw.ttf.override) {
		// We have a new code page - update callibrated and pre-rendered data
		screen_font.PreRenderBlocks(vga.draw.ttf.block_width, vga.draw.ttf.block_height);
		return;
	}

	if (!should_override_screen && !vga.draw.ttf.override) {
		// Nothing to do, TTF output should stay disabled
		return;
	}

	// Either we should take over or back away
	// Either way we need to re-create drawing configuration
	VGA_SetupDrawing(0);
}

// ***************************************************************************
// Configuration
// ***************************************************************************

// Check if the file is suitable for loading
static bool is_font_file_ok(const std_fs::path& candidate) {
	if (candidate.empty() || !std_fs::exists(candidate)) {
		return false;
	}

	if (std_fs::is_regular_file(candidate)) {
		return true;
	}

	if (std_fs::is_symlink(candidate) &&
	    std_fs::is_regular_file(std::filesystem::read_symlink(candidate))) {
		return true;
	}

	return false;
}

static std::string get_file_name(const std_fs::path& path) // XXX this should be general
{
	auto count = std::distance(path.begin(), path.end());
	if (count < 1 || !path.has_filename()) {
		return {};
	}

	for (const auto &current : path) {
		if ((--count) == 0) {
			return current;
		}
	}

	return {};
}

static std_fs::path find_font_file(const std_fs::path& root_path, const std::string& file_name, const uint8_t max_depth = 3)
{
	if (!std_fs::is_directory(root_path)) {
		return {};
	}

	std_fs::path result = {};

	// Look for files in the current directory
	for (const auto& dir_entry : std_fs::recursive_directory_iterator(root_path))
	{
		if (!dir_entry.is_regular_file()) {
			continue;
		}

#if defined(WIN32) || defined(MACOSX)
		// Windows, MacOS - case insensitive file name compare
		if (iequals(get_file_name(dir_entry.path()), file_name)) {
#else
		// Linux - sensitive file name compare
		if (get_file_name(dir_entry.path()) == file_name) {
#endif
			return dir_entry.path();
		}
	}

	if (max_depth == 0) {
		return result;
	}

	// Look in subdirectories
	for (const auto& dir_entry : std_fs::recursive_directory_iterator(root_path))
	{
		if (!dir_entry.is_directory()) {
			continue;
		}

		const auto result = find_font_file(dir_entry.path(), file_name, max_depth - 1);
		if (!result.empty() && is_font_file_ok(result)) {
			// Found the file in one of the subdirectories
			return result;
		}
	}

	return result;
}

static std_fs::path find_default_font_file()
{
	const auto result = get_resource_path(ResourceDir, DefaultFont);
	if (result.empty()) {
		LOG_ERR("TTF: Could not find the default font");
	}
	return result;
}

static std_fs::path find_custom_font_file(const std::string& font_name)
{
	if (font_name.empty()) {
		return {};
	}

	// Return file name with standard font extension
	auto get_path_with_extension = [](const std_fs::path &path) {
		if (get_file_name(path).contains('.')) {
			return path;
		}

		const std::string DefaultExtension = ".ttf";

		return std_fs::path(path.string() + DefaultExtension);
	};

	// Check for the file precisely as specified in the config file
	const std_fs::path native_path = to_native_path(font_name);
	if (is_font_file_ok(native_path)) {
		return native_path;
	}

	// Maybe try with the standard extension added
	const bool has_extension = get_file_name(native_path).contains('.');
	if (!has_extension) {
		const auto native_path_extension = get_path_with_extension(native_path);
		if (is_font_file_ok(native_path_extension)) {
			return native_path_extension;
		}
	}
	// Check if we should try the standard system directories
	const std_fs::path font_file_name = font_name;
	if (std::distance(font_file_name.begin(), font_file_name.end()) == 1) {
		// We have a bare file name, without path - check the standard font locations

		const auto directories = get_standard_font_dirs();
		for (const auto &directory : directories) {
			const auto result = find_font_file(directory, font_file_name);
			if (!result.empty()) {
				return result;
			}
		}

		if (!has_extension) {
			// Not found - try once again, with the standard extension added
			const auto font_file_name_extension = get_path_with_extension(font_file_name);
			for (const auto &directory : directories) {
				const auto result = find_font_file(directory, font_file_name_extension);
				if (!result.empty()) {
					return result;
				}
			}
		}
	}

	LOG_WARNING("TTF: Could not find the '%s' font", font_name.c_str());
	return {};
}

static void maybe_recalculate_drawing() // XXX find a better location
{
	if (!is_initialized || !is_window_ready() || !is_supported_text_mode()) {
		return;
	}

#ifndef DEBUG_TTF_NO_SCREEN_CACHE
	screen_cache.MarkAllDirty();
#endif

	PIC_RemoveEvents(VGA_SetupDrawing);
	PIC_AddEvent(VGA_SetupDrawing, 0);
}

void TRUETYPE_ReadConfigFont(SectionProp& section)
{
	const auto old_value = screen_font_file;

	screen_font_file = section.GetString("ttf_font");
	if (is_initialized && old_value == screen_font_file) {
		return;
	}

	// Get the custom font path
	const auto custom_font_path = find_custom_font_file(screen_font_file);

	// Load the custom specified font
	if (!custom_font_path.empty() && screen_font.Load(custom_font_path)) {
		// Custom specified font loaded succesfully
		LOG_INFO("TTF: Loaded font '%s'", custom_font_path.c_str());
		maybe_recalculate_drawing();
		return;
	}

	// Get the default font path
	const auto default_font_path = find_default_font_file();
	if (default_font_path.empty()) {
		maybe_recalculate_drawing();
		return;
	}

	// Load the default font
	if (screen_font.Load(default_font_path)) {
		LOG_INFO("TTF: Loaded default font '%s'", DefaultFont.c_str());
	}
	maybe_recalculate_drawing();
}

void TRUETYPE_ReadConfigOutput(SectionProp& section)
{
	const auto old_value = is_ttf_enabled;

	is_ttf_enabled = section.GetBool("ttf_output");

	if (is_ttf_enabled != old_value) {
		maybe_recalculate_drawing();
	}
}

void TRUETYPE_ReadConfigAspect(SectionProp& section)
{
	const auto old_value = aspect_mode;

	const auto aspect_mode_str = section.GetString("ttf_aspect");
	if (aspect_mode_str == "original") {
		aspect_mode = AspectMode::Original;
	} else if (aspect_mode_str == "wide") {
		aspect_mode = AspectMode::Wide;
	} else if (aspect_mode_str == "font") {
		aspect_mode = AspectMode::Font;
	}

	if (old_value != aspect_mode) {
		maybe_recalculate_drawing();
	}
}

static void read_config()
{
	const auto section = get_sdl_section();
	assert(section);
	if (section == nullptr) {
		return;
	}

	TRUETYPE_ReadConfigOutput(*section);
	TRUETYPE_ReadConfigAspect(*section);
	TRUETYPE_ReadConfigFont(*section);
}

void TRUETYPE_AddConfigOptions(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("ttf_output", Always, true);
	pbool->SetHelp("Replace text mode output with TrueType font output ('on' by default).\n"
	                "\n"
	                "Notes:\n"
			"  - CGA composite output is not supported.\n"
	                "\n"
			"  - Code pages loaded from custom CPI files are not supported.\n"
			"\n"
			"  - Concrete code page support depends on the selected font."
	);

	auto pstring = section.AddString("ttf_aspect", Always, "wide");
	pstring->SetValues({"original", "wide", "font"}); // XXX constants for strings
	pstring->SetHelp("Selects the TrueType font aspect ratio ('wide' by default):\n"
	                 "\n"
	                 "  original:  Keeps the original CRT monitor 4:3 screen proportions.\n"
	                 "\n"
	                 "  wide:      Similar 'original', but more widescreen friendly; for text modes\n"
	                 "             above 80 characters wide, the screen width is extended\n"
                         "             proportionally (default).\n"
                         "\n"
	                 "  font:      Keeps the selected TrueType font proportions.\n"
		         "\n"
		         "Note: Set 'aspect = stretch' to strech the image to viewport dimenions.");

	pstring = section.AddString("ttf_font", Always, "");
	pstring->SetHelp("Name of the TrueType font file to be used for text display (default empty).\n"
                         "The '.ttf' file extension can be omitted. If no path is provided, standard font\n"
                         "locations are searched in addition to the current directory. If empty, the\n"
                         "default bundled font is used.");
}

// ***************************************************************************
// Information retrieval
// ***************************************************************************

bool TRUETYPE_IsOverridingScreen()
{
	return vga.draw.ttf.override;
}

std::string TRUETYPE_GetLoadedScreenFont()
{
	return get_file_name(screen_font.LoadedFilePath());
}

// XXX this function needs unit tests
std::string TRUETYPE_ShortenFontName(const std::string font_name, const size_t max_length)
{
	const std::string TrimMark = "(...)";
	const auto target_length = std::max(max_length, TrimMark.size());

	auto result = font_name;
	if (result.size() <= target_length) {
		// No need to trim the name
		return result;
	}

	// If no extension present, just trim the trailing part
	if (!result.contains('.')) {
		result.resize(target_length - TrimMark.size());
		return result + TrimMark;
	}

	// Split the file name into stem and extension
	const auto split_point = result.find_last_of('.');

	auto result_stem      = result.substr(0, split_point);
	auto result_extension = result.substr(split_point + 1);

	// If extension is really long, just trim the trailing part
	const auto extension_length   = result_extension.length();
	const auto target_stem_length = target_length - extension_length - 1;
	if (TrimMark.length() > target_stem_length) {
		result.resize(target_length - TrimMark.size());
		return result + TrimMark;
	}

	// Shorten the stem, keep the extension intact
	result_stem.resize(target_stem_length - TrimMark.size());
	return result_stem + TrimMark + '.' + result_extension;
}

// ***************************************************************************
// Lifecycle
// ***************************************************************************

void TRUETYPE_Init()
{
	if (is_initialized) {
		return;
	}

	if (FT_Err_Ok != FT_Init_FreeType(&library)) {
		LOG_ERR("TRUETYPE: Error initializing FreeType library");
		// XXX implement reaction
	}

	read_config();
	is_initialized = true;
}

void TRUETYPE_Shutdown()
{
	screen_font.Unload();
#ifndef DEBUG_TTF_NO_SCREEN_CACHE
	screen_cache.FreeMemory();
#endif
	FT_Done_FreeType(library);
	is_initialized = false;
}
