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

#include "dosbox.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "checks.h"
#include "dos_inc.h"
#include "string_utils.h"

CHECK_NARROWING();


class Grapheme final {

public:

    Grapheme();
    Grapheme(const uint16_t code_point);

    bool IsEmpty() const;
    bool IsValid() const;
    bool HasMark() const;
    uint16_t GetCodePoint() const;

    void Invalidate();
    void AddMark(const uint16_t code_point);
    void StripMarks();

    bool operator<(const Grapheme &other) const;

private:

    // Unicode code point
    uint16_t code_point = static_cast<uint16_t>(' ');
    // Combining marks, 0 = none, 1 = U+0300, 2 = U+0301, etc.
    uint8_t  mark_1     = 0;
    uint8_t  mark_2     = 0;

    bool empty = true;
    bool valid = true;
};


typedef std::map<Grapheme, uint8_t>  CodePageMapping; // UTF-8 -> code page
typedef std::map<uint8_t, Grapheme>  CodePageMappingReverse;

typedef std::map<uint16_t, uint16_t> ConfigDuplicates;
typedef std::vector<std::pair<uint16_t, uint16_t>> ConfigAliases;

typedef struct ConfigMappingEntry {
    bool valid = false;
    CodePageMappingReverse mapping = {};
    uint16_t extends_code_page = 0;
    std::string extends_dir    = "";
    std::string extends_file   = "";
} ConfigMappingEntry;

typedef std::map<uint16_t, ConfigMappingEntry> ConfigMappings;


static const std::string file_name_main   = "MAIN.TXT";
static const std::string file_name_ascii  = "ASCII.TXT";
static const std::string dir_name_mapping = "mapping";

// Use the character below if there is absolutely no sane way to handle UTF-8 glyph
constexpr uint8_t unknown_character = 0x3f; // '?'

// First and last code point of the combining marks
constexpr uint16_t combining_first = 0x300;
constexpr uint16_t combining_last  = 0x36f;

// Main information about how to create UTF-8 mappings for given DOS code page
static ConfigMappings   config_mappings = {};
// UTF-8 -> UTF-8 fallback mapping (alias), use before fallback to 7-bit ASCII
static ConfigAliases    config_aliases = {};
// Information about code pages which are exact duplicates
static ConfigDuplicates config_duplicates = {};

// UTF-8 -> 7-bit ASCII mapping, use as a last resort mapping
static CodePageMapping  mapping_ascii = {};

// Concrete UTF-8 -> codepage mappings
static std::map<uint16_t, CodePageMapping> mappings = {};
// Additional UTF-8 -> codepage mappings, to avaoid unknown characters
static std::map<uint16_t, CodePageMapping> mappings_aliases = {};
// Reverse mappings, codepage -> UTF-8
static std::map<uint16_t, CodePageMappingReverse> mappings_reverse = {};


// ***************************************************************************
// Grapheme type implementation
// ***************************************************************************

static bool is_combining_mark(const uint32_t code_point)
{
    return (code_point >= combining_first) &&
           (code_point <= combining_last);
}

Grapheme::Grapheme()
{
}

Grapheme::Grapheme(const uint16_t code_point) :
    code_point(code_point),
    empty(false)
{
    // It is not valid to have a combining mark
    // as a main code point of the grapheme

    if (is_combining_mark(code_point))
        Invalidate();
}

bool Grapheme::IsEmpty() const
{
    return empty;
}

bool Grapheme::IsValid() const
{
    return valid;
}

bool Grapheme::HasMark() const
{
    return mark_1;
}

uint16_t Grapheme::GetCodePoint() const
{
    return code_point;
}

void Grapheme::Invalidate()
{
    empty = false;
    valid = false;

    code_point = unknown_character;
    mark_1     = 0;
    mark_2     = 0;
}

void Grapheme::AddMark(const uint16_t code_point)
{
    if (!valid)
        return; // can't add combining mark to invalid grapheme

    if (!is_combining_mark(code_point) || // not a combining mark
        empty ||  // can't add combining mark an to empty grapheme
        mark_2) { // can't add more than 2 combining marks, not supported

        Invalidate();
        return;
    }

    const uint8_t mark = static_cast<uint8_t>(code_point - combining_first + 1);

    if (mark_1)
        mark_2 = mark;
    else
        mark_1 = mark;
}

void Grapheme::StripMarks()
{
    mark_1 = 0;
    mark_2 = 0;
}

bool Grapheme::operator<(const Grapheme &other) const
{
    // TODO: after migrating to C++20 use a templated lambda here

    if (code_point < other.code_point)
        return true;
    else if (code_point > other.code_point)
        return false;

    if (mark_1 < other.mark_1)
        return true;
    else if (mark_1 > other.mark_1)
        return false;

    if (mark_2 < other.mark_2)
        return true;
    else if (mark_2 > other.mark_2)
        return false;

    if (empty < other.empty)
        return true;
    else if (empty > other.empty)
        return false;

    if (valid < other.valid)
        return true;
    else if (valid > other.valid)
        return false;

    return false;
}

// ***************************************************************************
// Conversion routines
// ***************************************************************************

static uint8_t char_to_uint8(const char in_char)
{
    if (in_char >= 0)
        return static_cast<uint8_t>(in_char);
    else
        return static_cast<uint8_t>(UINT8_MAX + in_char + 1);
}

static char uint8_to_char(const uint8_t in_uint8)
{
    if (in_uint8 <= INT8_MAX)
        return static_cast<char>(in_uint8);
    else
        return static_cast<char>(in_uint8 - INT8_MIN + INT8_MIN);
}

static void utf8_to_wide(const std::string &str_in, std::vector<uint16_t> &str_out)
{
    // Convert UTF-8 string to a sequence of decoded integers

    // For UTF-8 encoding explanation see here:
    // - https://www.codeproject.com/Articles/38242/Reading-UTF-8-with-C-streams
    // - https://en.wikipedia.org/wiki/UTF-8#Encoding

    str_out.clear();
    for (size_t i = 0; i < str_in.size(); i++) {
        const size_t  remaining = str_in.size() - i - 1;
        const uint8_t byte_1 = char_to_uint8(str_in[i]);
        const uint8_t byte_2 = (remaining >= 1) ? char_to_uint8(str_in[i + 1]) : 0;
        const uint8_t byte_3 = (remaining >= 2) ? char_to_uint8(str_in[i + 2]) : 0;

        // Retrieve code point
        uint32_t code_point = UINT32_MAX; // dummy, to trigger unsupported
        if (byte_1 < 0x80) {

            code_point = byte_1; // 1-byte code point, ASCII compatible

        } else if (GCC_UNLIKELY(byte_1 < 0xc0)) {

            code_point = unknown_character; // not an UTF-8 code point

        } else if (GCC_UNLIKELY(byte_1 >= 0xfc)) {

            i += 5; // 6-byte code point (>= 31 bits), no support

        } else if (GCC_UNLIKELY(byte_1 >= 0xf8)) {

            i += 4; // 5-byte code point (>= 26 bits), no support

        } else if (GCC_UNLIKELY(byte_1 >= 0xf0)) {

            i += 3; // 4-byte code point (>= 21 bits), no support

        } else if (GCC_UNLIKELY(byte_1 >= 0xe0)) {

            i += 2; // 3-byte code point

            if (byte_2 >= 0x80 && byte_2 < 0xc0 &&
                byte_3 >= 0x80 && byte_3 < 0xc0) {
                code_point = static_cast<uint8_t>(byte_1 - 0xe0);
                code_point = code_point << 6;
                code_point = code_point + byte_2 - 0x80;
                code_point = code_point << 6;
                code_point = code_point + byte_3 - 0x80;
            }

        } else {

            i += 1; // 2-byte codepoint

            if (byte_2 >= 0x80 && byte_2 < 0xc0) {
                code_point = static_cast<uint8_t>(byte_1 - 0xc0);
                code_point = code_point << 6;
                code_point = code_point + byte_2 - 0x80;
            }
        }

        if (code_point <= UINT16_MAX)
            // Latin, Greek, Cyrillic, Hebrew, Arabic, etc.
            // VGA charset symbols are in this range, too
            str_out.push_back(static_cast<uint16_t>(code_point));
        else {
            // Chinese, Japanese, Korean, historic scripts, emoji, etc.
            // No support for these, useless for DOS emulation
            str_out.push_back(unknown_character);
        }
    }
}

static void warn_code_point(const uint16_t code_point)
{
    static std::set<uint16_t> already_warned;
    if (already_warned.count(code_point))
        return;
    already_warned.insert(code_point);
    LOG_WARNING("UTF8: No fallback mapping for code point 0x%04x", code_point);
}

static void warn_code_page(const uint16_t code_page)
{
    static std::set<uint16_t> already_warned;
    if (already_warned.count(code_page))
        return;
    already_warned.insert(code_page);
    LOG_WARNING("UTF8: Requested unknown code page %d", code_page);
}

static void wide_to_code_page(const std::vector<uint16_t> &str_in,
                              std::string &str_out, const uint16_t code_page)
{
    CodePageMapping *mapping = nullptr;
    CodePageMapping *mapping_aliases = nullptr;

    // Try to find UTF8 -> code page mapping
    if (code_page != 0) {
        const auto it = mappings.find(code_page);
        if (it != mappings.end())
            mapping = &it->second;
        else
            warn_code_page(code_page);

        const auto it_alias = mappings_aliases.find(code_page);
        if (it_alias != mappings_aliases.end())
            mapping_aliases = &it_alias->second;
    }

    // Handle code points which are 7-bit ASCII characters
    auto push_7bit = [&str_out](const Grapheme &grapheme) {
        if (grapheme.HasMark())
            return false;  // not a 7-bit ASCII character

        const auto code_point = grapheme.GetCodePoint();
        if (code_point >= 0x80)
            return false; // not a 7-bit ASCII character

        str_out.push_back(static_cast<char>(code_point));
        return true;
    };

    // Handle code points belonging to selected code page
    auto push_code_page = [&str_out](const CodePageMapping *mapping,
                                     const Grapheme &grapheme) {
        if (!mapping)
            return false;
        const auto it = mapping->find(grapheme);
        if (it == mapping->end())
            return false;
        str_out.push_back(uint8_to_char(it->second));
        return true;
    };

    // Handle code points which can only be mapped to ASCII
    // using a fallback UTF8 mapping table
    auto push_fallback = [&str_out](const Grapheme &grapheme) {
        if (grapheme.HasMark())
            return false;
        const auto it = mapping_ascii.find(grapheme.GetCodePoint());
        if (it == mapping_ascii.end())
            return false;
        str_out.push_back(uint8_to_char(it->second));
        return true;
    };

    // Handle unknown code points
    auto push_unknown = [&str_out](const uint16_t code_point) {
        str_out.push_back(static_cast<char>(unknown_character));
        warn_code_point(code_point);
    };

    for (size_t i = 0; i < str_in.size(); i++)
    {
        Grapheme grapheme(str_in[i]);
        while (i + 1 < str_in.size() && is_combining_mark(str_in[i + 1])) {
            ++i;
            grapheme.AddMark(str_in[i]);
        }

        auto push = [&](const Grapheme &grapheme) {
            return push_7bit(grapheme) ||
                   push_code_page(mapping, grapheme) ||
                   push_code_page(mapping_aliases, grapheme) ||
                   push_fallback(grapheme);
        };

        if (push(grapheme))
            continue;

        if (grapheme.HasMark()) {
            grapheme.StripMarks();
            if (push(grapheme))
                continue;
        }

        push_unknown(grapheme.GetCodePoint());
    }
}

// ***************************************************************************
// Read resources from files
// ***************************************************************************

static bool prepare_code_page(const uint16_t code_page);

template <typename T1, typename T2>
bool add_if_not_mapped(std::map<T1, T2> &mapping, T1 first, T2 second)
{
    if (mapping.count(first))
        return false;

    mapping[first] = second;
    return true;
}

static void strip_line(std::string &line)
{
    // Strip comments
    const auto pos1 = line.find('#');
    if (pos1 != std::string::npos)
        line.resize(pos1);

    // Strip trailing whitespaces
    const auto pos2 = line.find_last_not_of(" \t");
    if (pos2 != std::string::npos)
        line.resize(pos2 + 1);
    else
        line.clear();
}

static std::ifstream open_mapping_file(const std_fs::path &path_root,
                                       const std::string &file_name)
{
    const std_fs::path file_path = path_root / file_name;   
    std::ifstream in_file(file_path.string());
    if (!in_file) {
        LOG_ERR("UTF8: Could not open mapping file %s", file_name.c_str());
    }

    return in_file;
}

static bool get_mapping_line(std::ifstream &in_file,
                             std::string &line_str,
                             size_t &line_num)
{
    line_str.clear();

    while (line_str.empty()) {
        if (!std::getline(in_file, line_str))
            return false;
        line_num++;
        strip_line(line_str);
    }

    return true;
}

static uint16_t get_code_page(const std::string &str)
{
    const auto tmp = std::atoi(str.c_str());
    if (tmp <= UINT16_MAX)
        return static_cast<uint16_t>(tmp);
    return 0;
}

static void error_parsing(const std::string &file_name,
                          const size_t line_num,
                          const std::string &details = "")
{
    if (details.empty())
        LOG_ERR("UTF8: Error parsing mapping file %s, line %d",
                file_name.c_str(),
                static_cast<int>(line_num));
    else
        LOG_ERR("UTF8: Error parsing mapping file %s, line %d: %s",
                file_name.c_str(),
                static_cast<int>(line_num),
                details.c_str());
}

static void error_code_page_invalid(const std::string &file_name,
                                    const size_t line_num)
{
    error_parsing(file_name, line_num, "invalid code page number");
}

static void error_code_page_defined(const std::string &file_name,
                                    const size_t line_num)
{
    error_parsing(file_name, line_num, "code page already defined");
}

static void error_code_page_none(const std::string &file_name,
                                 const size_t line_num)
{
    error_parsing(file_name, line_num, "not currently defining a code page");
}

static bool check_import_status(const std::ifstream &in_file,
                                const std::string &file_name,
                                const bool empty)
{
    if (in_file.fail() && !in_file.eof()) {
        LOG_ERR("UTF8: Error reading mapping file %s", file_name.c_str());
        return false;
    }

    if (empty) {
        LOG_ERR("UTF8: Mapping file %s has no entries", file_name.c_str());
        return false;
    }

    return true;
}

static bool check_grapheme_valid(const Grapheme &grapheme,
                                 const std::string &file_name,
                                  const size_t line_num)
{
    if (grapheme.IsValid())
        return true;

    LOG_ERR("UTF8: Error, invalid grapheme defined in file %s, line %d",
            file_name.c_str(), static_cast<int>(line_num));
    return false;    
}

static const std::regex regex_map_ascii(
    // hex_4_digits NNN|CPS|HSH|character
    "^0x([[:xdigit:]]{4})[[:space:]]+.*(NNN|SPC|HSH|[^[:space:]])$",
    std::regex_constants::optimize);

static const std::regex regex_map_code(
    // hex_2_digits hex_4_digits
    "^0x([[:xdigit:]]{2})[[:space:]]+0x([[:xdigit:]]{4})$",
    std::regex_constants::optimize);

static const std::regex regex_map_code_mark1(
    // hex_2_digits hex_4_digits
    "^0x([[:xdigit:]]{2})[[:space:]]+0x([[:xdigit:]]{4})[[:space:]]+0x([[:xdigit:]]{4})$",
    std::regex_constants::optimize);

static const std::regex regex_map_code_mark2(
    // hex_2_digits hex_4_digits
    "^0x([[:xdigit:]]{2})[[:space:]]+0x([[:xdigit:]]{4})[[:space:]]+0x([[:xdigit:]]{4})[[:space:]]+0x([[:xdigit:]]{4})$",
    std::regex_constants::optimize);

static const std::regex regex_map_undefined(
    // hex_2_digits hex_4_digits
    "^0x[[:xdigit:]]{2}$",
    std::regex_constants::optimize);

static const std::regex regex_map_alias(
    // ALIAS hex_4_digits hex_4_digits [BIDIRECTIONAL]
    "^ALIAS[[:space:]]+0x([[:xdigit:]]{4})[[:space:]]+0x([[:xdigit:]]{4})([[:space:]]+BIDIRECTIONAL)?$",
    std::regex_constants::optimize);

static const std::regex regex_cp_start(
    // CODEPAGE dec
    "^CODEPAGE[[:space:]]+([[:digit:]]{1,5})$",
    std::regex_constants::optimize);

static const std::regex regex_cp_duplicates(
    // CODEPAGE dec DUPLICATES dec
    "^CODEPAGE[[:space:]]+([[:digit:]]{1,5})[[:space:]]+DUPLICATES[[:space:]]+([[:digit:]]{1,5})$",
    std::regex_constants::optimize);

static const std::regex regex_extend_file(
    // EXTENDS FILE dir_name file_NAME
    "^EXTENDS[[:space:]]+FILE[[:space:]]+([^[:space:]]+)[[:space:]]+([^[:space:]]+)$",
    std::regex_constants::optimize);

static const std::regex regex_extend_cp(
    // EXTENDS CODEPAGE dec
    "^EXTENDS[[:space:]]+CODEPAGE[[:space:]]+([[:digit:]]+)$",
    std::regex_constants::optimize);


static bool import_mapping_code_page(const std_fs::path &path_root,
                                     const std::string &file_name,
                                     CodePageMappingReverse &mapping)
{
    // Import code page character -> UTF-8 mapping from external file

    // Open the file
    auto in_file = open_mapping_file(path_root, file_name);
    if (!in_file) {
        LOG_ERR("UTF8: Error opening mapping file %s", file_name.c_str());
        return false;
    }

    // Read and parse
    std::string line_str = " ";
    size_t      line_num = 0;
    std::smatch match;

    CodePageMappingReverse new_mapping;

    while (get_mapping_line(in_file, line_str, line_num)) {

        if (line_str.length() == 1 && line_str[0] == 0x1a)
            break; // end of file marking, for some fiels from unicode.org

        auto get_hex_match = [&match](const uint8_t i) {
            return std::strtol(match.str(i).c_str(), nullptr, 16);
        };

        if (std::regex_match(line_str, match, regex_map_code) ||
            std::regex_match(line_str, match, regex_map_code_mark1) ||
            std::regex_match(line_str, match, regex_map_code_mark2)) {

            // Handle mapping entry

            const auto character_code = static_cast<uint8_t>(get_hex_match(1));

            if (character_code >= 0x80) { // ignore codes below 0x80
                const auto code_point = static_cast<uint16_t>(get_hex_match(2));

                Grapheme grapheme(code_point);
                if (match.size() > 3)
                    grapheme.AddMark(static_cast<uint16_t>(get_hex_match(3)));
                if (match.size() > 4)
                    grapheme.AddMark(static_cast<uint16_t>(get_hex_match(4)));

                // Invalid grapheme that is not added (overridden) is OK here;
                // at least CP 1258 definition from Unicode.org contains mapping
                // of code page characters to combining marks, which is fine
                // for converting texts, but a no-no for DOS emulation (where
                // the number of output characters has to match the number of
                // input characters).
                // For such code page definitions, just override problematic
                // mappings in the main mapping configuration file.

                if (add_if_not_mapped(new_mapping, character_code, grapheme) &&
                    !check_grapheme_valid(grapheme, file_name, line_num))
                    return false;
            }

        } else if (std::regex_match(line_str, match, regex_map_undefined)) {

            // Handle undefined character entry

            const auto character_code = static_cast<uint8_t>(get_hex_match(1));
            if (character_code >= 0x80)  { // ignore codes below 0x80
                Grapheme grapheme;
                add_if_not_mapped(new_mapping, character_code, grapheme);
            }

        } else {
            error_parsing(file_name, line_num);
            return false;
        }
    }

    if (!check_import_status(in_file, file_name, new_mapping.empty()))
        return false;

    // Reading/parsing succeeded - use all the data read from file
    mapping = new_mapping;
    return true;
}

static void import_config_main(const std_fs::path &path_root)
{
    // Import main configuration file, telling how to construct UTF-8 mappings
    // for each and every supported code page

    // Open the file
    auto in_file = open_mapping_file(path_root, file_name_main);
    if (!in_file)
        return;

    // Read and parse
    bool file_empty      = true;
    std::string line_str = " ";
    size_t      line_num = 0;
    std::smatch match;

    uint16_t curent_code_page = 0;
    ConfigMappings   new_config_mappings;
    ConfigDuplicates new_config_duplicates;
    ConfigAliases    new_config_aliases;

    while (get_mapping_line(in_file, line_str, line_num)) {

        auto get_hex_match = [&match](const uint8_t i) {
            return std::strtol(match.str(i).c_str(), nullptr, 16);
        };

        if (std::regex_match(line_str, match, regex_map_code) ||
            std::regex_match(line_str, match, regex_map_code_mark1) ||
            std::regex_match(line_str, match, regex_map_code_mark2)) {
           
            // Handle mapping entry

            if (!curent_code_page) {
                error_code_page_none(file_name_main, line_num);
                return;
            }

            auto &new_mapping = new_config_mappings[curent_code_page].mapping;
            const auto character_code = static_cast<uint8_t>(get_hex_match(1));

            if (character_code >= 0x80) {  // ignore codes below 0x80

                const auto code_point = static_cast<uint16_t>(get_hex_match(2));
                Grapheme grapheme(code_point);
                if (match.size() > 3)
                    grapheme.AddMark(static_cast<uint16_t>(get_hex_match(3)));
                if (match.size() > 4)
                    grapheme.AddMark(static_cast<uint16_t>(get_hex_match(4)));

                if (!check_grapheme_valid(grapheme, file_name_main, line_num))
                    return;

                add_if_not_mapped(new_mapping, character_code, grapheme);
                file_empty = false; // some meningful mapping provided
            }

        } else if (std::regex_match(line_str, match, regex_map_undefined)) {

            // Handle undefined character entry

            if (!curent_code_page) {
                error_code_page_none(file_name_main, line_num);
                return;
            }

            auto &new_mapping = new_config_mappings[curent_code_page].mapping;
            const auto character_code = static_cast<uint8_t>(get_hex_match(1));

            if (character_code >= 0x80) {  // ignore codes below 0x80
                Grapheme grapheme;
                add_if_not_mapped(new_mapping, character_code, grapheme);
                file_empty = false; // some meningful mapping provided
            }

        } else if (std::regex_match(line_str, match, regex_map_alias)) {

            // Handle ALIAS

            const auto code_point_1 = static_cast<uint16_t>(get_hex_match(1));
            const auto code_point_2 = static_cast<uint16_t>(get_hex_match(2));

            new_config_aliases.push_back(std::make_pair(code_point_1, code_point_2));

            if (match.str(3).size() != 0) // check if bidirectional
                new_config_aliases.push_back(std::make_pair(code_point_2, code_point_1));

            curent_code_page = 0;

        } else if (std::regex_match(line_str, match, regex_cp_start)) {

            // Handle CODEPAGE

            const auto cp = get_code_page(match.str(1));
            if (!cp) {
                error_code_page_invalid(file_name_main, line_num);
                return;
            }

            if (new_config_mappings[cp].valid) {
                error_code_page_defined(file_name_main, line_num);
                return;
            }

            new_config_mappings[cp].valid = true;
            curent_code_page = cp;

        } else if (std::regex_match(line_str, match, regex_cp_duplicates)) {

            // Handle CODEPAGE ... DUPLICATES

            const auto cp_1 = get_code_page(match.str(1));
            const auto cp_2 = get_code_page(match.str(2));

            if (!cp_1 || !cp_2) {
                error_code_page_invalid(file_name_main, line_num);
                return;
            }

            new_config_duplicates[cp_1] = cp_2;
            curent_code_page = 0;

        } else if (std::regex_match(line_str, match, regex_extend_file)) {

            // Handle EXTENDS FILE

            if (!curent_code_page) {
                error_code_page_none(file_name_main, line_num);
                return;
            }

            new_config_mappings[curent_code_page].extends_dir  = match.str(1);
            new_config_mappings[curent_code_page].extends_file = match.str(2);
            file_empty = false; // some meningful mapping provided
            curent_code_page = 0;

        } else if (std::regex_match(line_str, match, regex_extend_cp)) {

            // Handle EXTENDS CODEPAGE

            if (!curent_code_page) {
                error_code_page_none(file_name_main, line_num);
                return;
            }

            const auto cp = get_code_page(match.str(1));
            if (!cp) {
                error_code_page_invalid(file_name_main, line_num);
                return;
            }

            new_config_mappings[curent_code_page].extends_code_page = cp;
            curent_code_page = 0;

        } else {
            error_parsing(file_name_main, line_num);
            return; 
        }
    }

    if (!check_import_status(in_file, file_name_main, file_empty))
        return;

    // Reading/parsing succeeded - use all the data read from file
    config_mappings   = new_config_mappings;
    config_duplicates = new_config_duplicates;
    config_aliases    = new_config_aliases;
}

static void import_mapping_ascii(const std_fs::path &path_root)
{
    // Import fallback mapping, from UTH-8 to 7-bit ASCII;
    // this mapping will only be used if everything else fails

    // Open the file
    auto in_file = open_mapping_file(path_root, file_name_ascii);
    if (!in_file)
        return;

    // Read and parse
    std::string line_str = "";
    size_t      line_num = 0;
    std::smatch match;

    CodePageMapping new_mapping_ascii;

    while (get_mapping_line(in_file, line_str, line_num)) {

        if (line_str.length() == 1 && line_str[0] == 0x1a)
            break; // end of file

        // Check if line matches the pattern
        if (!std::regex_match(line_str, match, regex_map_ascii)) {
            error_parsing(file_name_ascii, line_num);
            return; 
        }

        // Create a mapping entry
        const auto code_point = std::strtol(match.str(1).c_str(), nullptr, 16);
        const uint16_t idx    = static_cast<uint16_t>(code_point);

        if (match.str(2) == "NNN")
            new_mapping_ascii[idx] = unknown_character;
        else if (match.str(2) == "SPC")
            new_mapping_ascii[idx] = ' ';
        else if (match.str(2) == "HSH")
            new_mapping_ascii[idx] = '#';
        else
            new_mapping_ascii[idx] = char_to_uint8(match.str(2)[0]);
    }

    if (!check_import_status(in_file, file_name_ascii, new_mapping_ascii.empty()))
        return;

    // Reading/parsing succeeded - use the mapping
    mapping_ascii = new_mapping_ascii;
}

static uint16_t deduplicate_code_page(const uint16_t code_page)
{
    const auto it = config_duplicates.find(code_page);

    if (it == config_duplicates.end())
        return code_page;
    else
        return it->second;
}

static bool construct_mapping(const uint16_t code_page)
{
    // Prevent processing if previous attempt failed;
    // also protect against circular dependencies

    static std::set<uint16_t> already_tried;
    if (already_tried.count(code_page))
        return false;
    already_tried.insert(code_page);

    assert(!config_mappings.count(code_page));
    assert(!mappings.count(code_page));
    assert(!mappings_reverse.count(code_page));
    assert(!mappings.count(code_page));

    // First apply mapping found in main config file

    const auto &config_mapping = config_mappings[code_page];
    CodePageMapping new_mapping = {};
    CodePageMappingReverse new_mapping_reverse = {};

    auto add_to_mappings = [&](const uint8_t first, const Grapheme &second) {

        if (first < 0x80)
            return;

        if (!add_if_not_mapped(new_mapping_reverse, first, second))
            return;

        if (second.IsEmpty() || !second.IsValid())
            return;

        if (add_if_not_mapped(new_mapping, second, first))
            return;

        LOG_WARNING("UTF8: mapping for code page %d uses a code point twice; character 0x%02x",
                    code_page, first);
    };

    for (const auto &entry : config_mapping.mapping)
        add_to_mappings(entry.first, entry.second);

    // If code page is expansion of other code page, copy remaining entries

    if (config_mapping.extends_code_page) {

        const uint16_t dependency = deduplicate_code_page(config_mapping.extends_code_page);
        if (!prepare_code_page(dependency)) {
            LOG_ERR("UTF8: Code page %d mapping requires code page %d mapping",
                    code_page, dependency);
            return false;
        }

        for (const auto &entry : mappings[dependency])
            add_to_mappings(entry.second, entry.first);
    } 

    // If code page uses external mapping file, load appropriate entries

    if (!config_mapping.extends_file.empty()) {

        CodePageMappingReverse mapping_file;

        if (!import_mapping_code_page(GetResourcePath(config_mapping.extends_dir),
                                      config_mapping.extends_file,
                                      mapping_file))
            return false;

        for (const auto &entry : mapping_file)
            add_to_mappings(entry.first, entry.second);
    }

    mappings[code_page]         = new_mapping;
    mappings_reverse[code_page] = new_mapping_reverse;
    return true;
}

static void construct_mapping_aliases(const uint16_t code_page)
{
    assert(!mappings_aliases.count(code_page));
    assert(mappings.count(code_page));

    const auto &mapping = mappings[code_page];
    auto &mapping_aliases = mappings_aliases[code_page];

    for (const auto &alias : config_aliases)
        if (!mapping.count(alias.first) && mapping.count(alias.second) &&
            !mapping_aliases.count(alias.first))
            mapping_aliases[alias.first] = mapping.find(alias.second)->second;
}

static bool prepare_code_page(const uint16_t code_page)
{
    if (mappings.count(code_page))
        return true; // code page already prepared

    // If this is the first time we are requested to prepare the code page,
    // load the top-level configuration and fallback 7-bit ASCII mapping

    static bool config_loaded = false;
    if (!config_loaded) {
        const auto path_root = GetResourcePath(dir_name_mapping);
        import_mapping_ascii(path_root);
        import_config_main(path_root);
        config_loaded = true;
    }

    if (!config_mappings.count(code_page) || !construct_mapping(code_page)) {

        // Unsupported code page or error
        mappings.erase(code_page);
        mappings_reverse.erase(code_page);
        return false;
    }

    construct_mapping_aliases(code_page);
    return true;
}

// ***************************************************************************
// External interface
// ***************************************************************************

uint16_t UTF8_GetCodePage()
{
    const uint16_t cp_default = 437; // United States
    if (!IS_EGAVGA_ARCH)
        // Below EGA it wasn't possible to change character set
        return cp_default;

    const uint16_t cp = deduplicate_code_page(dos.loaded_codepage);

    // For unsupported code pages, revert to default one
    if (prepare_code_page(cp))
        return cp;
    else
        return cp_default;
}

void UTF8_RenderForDos(const std::string &str_in, std::string &str_out,
                       const uint16_t code_page)
{
    const uint16_t cp = deduplicate_code_page(code_page);
    prepare_code_page(cp);

    std::vector<uint16_t> str_wide;
    utf8_to_wide(str_in, str_wide);

    str_out.clear();
    wide_to_code_page(str_wide, str_out, cp);
    str_out.shrink_to_fit();
}


// If you need a routine to convert a screen code to UTF-8, here is how it should look like:

#if 0

// Note: this returns unencoded sequence of code points

void Grapheme::PushInto(std::vector<uint16_t> &str_out)
{
    str_out.push_back(code_point);

    if (!mark_1)
        return;
    str_out.push_back(mark_1 + combining_first - 1);

    if (!mark_2)
        return;
    str_out.push_back(mark_2 + combining_first - 1);
}

void UTF8_FromScreenCode(const uint8_t character_in, std::vector<uint16_t> &str_out,
                         const uint16_t code_page)
{
    str_out.clear();

    if (GCC_UNLIKELY(character_in >= 0x80)) {

        // Character above 0x07f - take from code page mapping

        const uint16_t cp = deduplicate_code_page(code_page);
        prepare_code_page(cp);

        if (!mappings_reverse.count(cp) ||
            !mappings_reverse[cp].count(character_in))
            str_out.push_back(' ');
        else
            (mappings_reverse[cp])[character_in].PushInto(str_out);

    } else {

        // Unicode code points for screen codes from 0x00 to 0x1f
        // see: https://en.wikipedia.org/wiki/Code_page_437
        constexpr uint16_t codes[0x20] = {
            0x0020, 0x263a, 0x263b, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, // 00-07
            0x25d8, 0x25cb, 0x25d9, 0x2642, 0x2640, 0x266a, 0x266b, 0x263c, // 08-0f
            0x25ba, 0x25c4, 0x2195, 0x203c, 0x00b6, 0x00a7, 0x25ac, 0x21a8, // 10-17
            0x2191, 0x2193, 0x2192, 0x2190, 0x221f, 0x2194, 0x25b2, 0x25bc, // 18-1f
        };

        if (GCC_UNLIKELY(character_in == 0x7f))
            str_out.push_back(0x2302);
        else if (character_in >= 0x20)
            str_out.push_back(character_in);
        else
            str_out.push_back(codes[character_in]);
    }

   str_out.shrink_to_fit();
}

#endif
