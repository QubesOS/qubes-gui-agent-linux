/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifndef QUBES_GUI_AGENT_ENCODING_H
#define QUBES_GUI_AGENT_ENCODING_H QUBES_GUI_AGENT_ENCODING_H

#include <stdbool.h>
void sanitize_string_from_vm(unsigned char *untrusted_s, int allow_utf8);
bool is_valid_clipboard_string_from_vm(unsigned char *untrusted_s);

#endif
