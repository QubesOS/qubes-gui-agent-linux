/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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
#include <stdlib.h>
#include <sys/mman.h>
#define xf86ValidateModes_ARGS void* scrp, void* availModes, char **modeNames, void* clockRanges,\
                               int *linePitches, int minPitch, int maxPitch, int pitchInc, int minHeight,\
                               int maxHeight, int virtualX, int virtualY, int apertureSize, int strategy

extern int xf86ValidateModes(xf86ValidateModes_ARGS);
int (*real_xf86ValidateModes) (xf86ValidateModes_ARGS);
int relaxed_xf86ValidateModes(xf86ValidateModes_ARGS)
{
	/* unused parameters */
	(void) maxPitch;
	(void) maxHeight;

	return real_xf86ValidateModes(scrp, availModes, modeNames,
				      clockRanges, linePitches, minPitch,
				      65536, pitchInc, minHeight, 65536,
				      virtualX, virtualY, apertureSize,
				      strategy);
}
#ifdef __x86_64
#define EXE_START 0x0000000000400000
#endif
#ifdef __i386__
#define EXE_START 0x08048000
#endif

int __attribute__ ((constructor)) initfunc()
{
	unsigned long *ptr = (unsigned long *) EXE_START;
	void *page_start;
	unsetenv("LD_PRELOAD");
	real_xf86ValidateModes = xf86ValidateModes;
	while ((*ptr) != (unsigned long) real_xf86ValidateModes)
		ptr++;
	page_start = (void *) ((unsigned long) ptr & (-4096UL));
	mprotect(page_start, 4096, PROT_READ | PROT_WRITE);
	*ptr = (unsigned long) relaxed_xf86ValidateModes;
	mprotect(page_start, 4096, PROT_READ | PROT_EXEC);
	return 0;
}
