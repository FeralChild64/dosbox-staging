// SPDX-FileCopyrightText:  2024-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_UNICODE_H
#define DOSBOX_UNICODE_H

#include <cstdint>
#include <string>
#include <vector>

// Get recommended DOS code page to render the UTF-8 strings to. This might not
// be the code page set using KEYB command, for example due to emulated hardware
// limitations, or duplicated code page numbers
uint16_t get_utf8_code_page();

// Returns the information whether the Unicode engine knows the given DOS code
// page and is able to use it for string conversion
bool is_code_page_supported(const uint16_t code_page);

// Specifies what to do if the DOS code page does not contain character
// representing given Unicode grapheme
enum class UnicodeFallback {

	// If any grapheme can't be converted without using a fallback
	// mechanism, return an empty string
	EmptyString,

	// Try to provide reasonable fallback using all the characters available
	// in target DOS code page; use for features like clipboard content
	// exchange with host system
	Simple,

	// Do not use certain DOS code page characters in order to draw boxes
	// (tables) which are consistent; for example, if code page contains
	// character '╠', but not '╣', both will be replaced with a fallback
	// character ('║' for example)
	Box
};

// Specifies how to interpret characters 0x00-0x1f and 0x7f in DOS strings
enum class DosStringConvertMode {

	// String contains control codes (new line, tabulation, delete, etc.)
	WithControlCodes,

	// String does not have any codes characters, consider all characters
	// as screen codes
	ScreenCodesOnly,

	// String should not contain characters mentioned above
	NoSpecialCharacters
};

// Convert the UTF-8 string to the format intended for display inside emulated
// environment, or vice-versa. Code page '0' means a pure 7-bit ASCII. Functions
// without 'code_page' parameters use current DOS code page.

std::string utf8_to_dos(const std::string& str,
                        const DosStringConvertMode convert_mode,
                        const UnicodeFallback fallback);

std::string utf8_to_dos(const std::string& str,
                        const DosStringConvertMode convert_mode,
                        const UnicodeFallback fallback,
                        const uint16_t code_page);

std::string dos_to_utf8(const std::string& str,
                        const DosStringConvertMode convert_mode);

std::string dos_to_utf8(const std::string& str,
                        const DosStringConvertMode convert_mode,
                        const uint16_t code_page);

// Convert the DOS format string to plain Unicode, without any encoding

std::u32string dos_to_unicode(const std::string& str,
                              const DosStringConvertMode convert_mode);

std::u32string dos_to_unicode(const std::string& str,
                              const DosStringConvertMode convert_mode,
                              const uint16_t code_page);

// Specialized routines for converting between code page 437 and UTF-8 without
// any combining marks - to be used in filesystem emulation

std::string fs_utf8_to_dos_437(const std::string& str);
std::string dos_437_to_fs_utf8(const std::string& str);

// Conversion between UTF-8 and UTF-16

std::string utf16_to_utf8(const std::u16string& str);
std::u16string utf8_to_utf16(const std::string& str);

// Conversion between UTF-8 and UCS-2 (used in FAT long file names)

std::string ucs2_to_utf8(const std::u16string& str);
std::u16string utf8_to_ucs2(const std::string& str);

// Convert DOS code page string to lower/upper case; converters are aware of all
// the national characters. Functions without 'code_page' parameter use current
// DOS code page.

std::string lowercase_dos(const std::string& str);
std::string lowercase_dos(const std::string& str, const uint16_t code_page);

std::string uppercase_dos(const std::string& str);
std::string uppercase_dos(const std::string& str, const uint16_t code_page);

// Calculate length of a UTF-8 encoded string
size_t length_utf8(const std::string& str);

// Check if two code pages are the same
bool is_code_page_equal(const uint16_t code_page1, const uint16_t code_page2);

// Check if the given code point belongs to a private area
bool is_code_point_private(const char32_t code_point);

// Check if the given code point is private, but well-known and used in at
// least one supported DOS code page
bool is_code_point_private_well_known(const char32_t code_point);

// Check if the given code point is private, assigned to a DOSBox-specific
// character (certain DOS code pages contain glyphs not available in the
// Unicode standard; DOSBox uses private code points to handle them)
bool is_code_point_dosbox_specific(const char32_t code_point);

#endif
