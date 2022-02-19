/*
 *  Copyright (C) 2022  The DOSBox Staging Team
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

#ifndef DOSBOX_VMWARE_H
#define DOSBOX_VMWARE_H

#include "dosbox.h"

extern volatile bool vmware_mouse;

void VMWARE_MouseButtonPressed(Bit8u button);
void VMWARE_MouseButtonReleased(Bit8u button);
void VMWARE_MousePosition(Bit32u pos_x, Bit32u pos_y, Bit32u res_x, Bit32u res_y);

#endif
