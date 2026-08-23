// SPDX-FileCopyrightText:  2026 dosbox-automation Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_RESTRICT_H
#define DOSBOX_RESTRICT_H

// Provides an equivalent of the C99 restrict type qualifier,
// which is not available in C++
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
// GCC, clang, Microsoft Visual C
#define restrict __restrict
#else
#warning Unsupported compiler
#define restrict
#endif

#endif // DOSBOX_RESTRICT_H
