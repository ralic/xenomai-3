/*
 * Copyright (C) 2013 Philippe Gerum <rpm@xenomai.org>.
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
#ifndef _LIB_COBALT_INIT_H
#define _LIB_COBALT_INIT_H

/*
 * We give the Cobalt library constructor a high priority, so that
 * extension libraries may assume the core services are available when
 * their own constructor runs. Priorities 0-100 may be reserved by the
 * implementation on some platforms, and we may want to keep some
 * levels free for very high priority inits, so pick 200.
 */
#define __LIBCOBALT_CTOR_PRIO  200

#define __libcobalt_ctor  __attribute__ ((constructor(__LIBCOBALT_CTOR_PRIO)))

void __libcobalt_init(void);

extern int __cobalt_defer_init;

extern int __cobalt_main_prio;

extern int __cobalt_print_bufsz;

#endif /* _LIB_COBALT_INIT_H */