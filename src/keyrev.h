/* live-f1
 *
 * Copyright © 2012 Yuriy Mishkov <ymishkov@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef LIVE_F1_KEYREV_H
#define LIVE_F1_KEYREV_H

#include "macros.h"
#include "packetdef.h"
/*#include "packetcache.h"*/


SJR_BEGIN_EXTERN

int reverse_key (unsigned int *key, const Packet *p);
/*int reverse_key (unsigned int *key, PacketIterator *from, const PacketIterator *to);*/

SJR_END_EXTERN

#endif /* LIVE_F1_KEYREV_H */
