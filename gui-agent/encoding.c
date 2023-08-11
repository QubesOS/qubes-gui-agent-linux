/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
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
#include "encoding.h"

/* validate single UTF-8 character
 * return bytes count of this character, or 0 if the character is invalid */
static int validate_utf8_char(unsigned char *untrusted_c) {
    int tails_count = 0;
    int total_size = 0;
    /* it is safe to access byte pointed by the parameter and the next one
     * (which can be terminating NULL), but every next byte can access only if
     * neither of previous bytes was NULL
     */

    /* According to http://www.ietf.org/rfc/rfc3629.txt:
     *   UTF8-char   = UTF8-1 / UTF8-2 / UTF8-3 / UTF8-4
     *   UTF8-1      = %x00-7F
     *   UTF8-2      = %xC2-DF UTF8-tail
     *   UTF8-3      = %xE0 %xA0-BF UTF8-tail / %xE1-EC 2( UTF8-tail ) /
     *                 %xED %x80-9F UTF8-tail / %xEE-EF 2( UTF8-tail )
     *   UTF8-4      = %xF0 %x90-BF 2( UTF8-tail ) / %xF1-F3 3( UTF8-tail ) /
     *                 %xF4 %x80-8F 2( UTF8-tail )
     *   UTF8-tail   = %x80-BF
     */

    if (*untrusted_c <= 0x7F) {
        return 1;
    } else if (*untrusted_c >= 0xC2 && *untrusted_c <= 0xDF) {
        total_size = 2;
        tails_count = 1;
    } else switch (*untrusted_c) {
        case 0xE0:
            untrusted_c++;
            total_size = 3;
            if (*untrusted_c >= 0xA0 && *untrusted_c <= 0xBF)
                tails_count = 1;
            else
                return 0;
            break;
        case 0xE1: case 0xE2: case 0xE3: case 0xE4:
        case 0xE5: case 0xE6: case 0xE7: case 0xE8:
        case 0xE9: case 0xEA: case 0xEB: case 0xEC:
            /* 0xED */
        case 0xEE:
        case 0xEF:
            total_size = 3;
            tails_count = 2;
            break;
        case 0xED:
            untrusted_c++;
            total_size = 3;
            if (*untrusted_c >= 0x80 && *untrusted_c <= 0x9F)
                tails_count = 1;
            else
                return 0;
            break;
        case 0xF0:
            untrusted_c++;
            total_size = 4;
            if (*untrusted_c >= 0x90 && *untrusted_c <= 0xBF)
                tails_count = 2;
            else
                return 0;
            break;
        case 0xF1:
        case 0xF2:
        case 0xF3:
            total_size = 4;
            tails_count = 3;
            break;
        case 0xF4:
            untrusted_c++;
            if (*untrusted_c >= 0x80 && *untrusted_c <= 0x8F)
                tails_count = 2;
            else
                return 0;
            break;
        default:
            return 0;
    }

    while (tails_count-- > 0) {
        untrusted_c++;
        if (!(*untrusted_c >= 0x80 && *untrusted_c <= 0xBF))
            return 0;
    }
    return total_size;
}

/* replace non-printable characters with '_'
 * given string must be NULL terminated already */
bool is_valid_clipboard_string_from_vm(unsigned char *untrusted_s)
{
    int utf8_ret;
    for (; *untrusted_s; untrusted_s++) {
        // allow only non-control ASCII chars
        if ((*untrusted_s >= 0x20 && *untrusted_s <= 0x7E) ||
             *untrusted_s == '\n' || *untrusted_s == '\t')
            continue;
        if (*untrusted_s >= 0x80) {
            utf8_ret = validate_utf8_char(untrusted_s);
            if (utf8_ret > 0) {
                /* loop will do one additional increment */
                untrusted_s += utf8_ret - 1;
                continue;
            }
        }
        return false;
    }
    return true;
}

/* replace non-printable characters with '_'
 * given string must be NULL terminated already */
void sanitize_string_from_vm(unsigned char *untrusted_s, int allow_utf8)
{
    int utf8_ret;
    for (; *untrusted_s; untrusted_s++) {
        // allow only non-control ASCII chars
        if (*untrusted_s >= 0x20 && *untrusted_s <= 0x7E)
            continue;
        if (allow_utf8 && *untrusted_s >= 0x80) {
            utf8_ret = validate_utf8_char(untrusted_s);
            if (utf8_ret > 0) {
                /* loop will do one additional increment */
                untrusted_s += utf8_ret - 1;
                continue;
            }
        }
        *untrusted_s = '_';
    }
}
