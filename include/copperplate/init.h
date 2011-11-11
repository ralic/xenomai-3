/*
 * Copyright (C) 2008 Philippe Gerum <rpm@xenomai.org>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */

#ifndef _COPPERPLATE_INIT_H
#define _COPPERPLATE_INIT_H

#include <stdarg.h>
#include <copperplate/core.h>
#include <copperplate/list.h>

struct copperskin {
	const char *name;
	int (*init)(int argc, char *const argv[]);
	struct pvholder next;
};

#ifdef __cplusplus
extern "C" {
#endif

void copperplate_init(int argc, char *const argv[]);

void copperplate_register_skin(struct copperskin *p);

void panic(const char *fmt, ...);

void warning(const char *fmt, ...);

const char *symerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* _COPPERPLATE_INIT_H */