/*
 * Copyright (C) 2005 Philippe Gerum <rpm@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _XENO_ASM_ARM_WRAPPERS_H
#define _XENO_ASM_ARM_WRAPPERS_H

#ifndef __KERNEL__
#error "Pure kernel header included from user-space!"
#endif

#include <linux/ipipe.h>
#include <linux/interrupt.h>
#include <asm-generic/xenomai/wrappers.h> /* Read the generic portion. */

#define wrap_phys_mem_prot(filp,pfn,size,prot)	(prot)

#define wrap_strncpy_from_user(dstP, srcP, n)	__strncpy_from_user(dstP, srcP, n)

#define __put_user_inatomic __put_user

#define __get_user_inatomic __get_user

#define rthal_irq_desc_status(irq)	(rthal_irq_descp(irq)->status)

#if !defined(CONFIG_GENERIC_HARDIRQS) \
	|| LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
#define rthal_irq_chip_enable(irq)   ({ rthal_irq_descp(irq)->chip->unmask(irq); 0; })
#define rthal_irq_chip_disable(irq)  ({ rthal_irq_descp(irq)->chip->mask(irq); 0; })
#endif
#define rthal_irq_desc_lock(irq)	(&rthal_irq_descp(irq)->lock)
#define rthal_irq_chip_end(irq)      ({ rthal_irq_descp(irq)->ipipe_end(irq, rthal_irq_descp(irq)); 0; })
typedef irq_handler_t rthal_irq_host_handler_t;
#define rthal_mark_irq_disabled(irq) do {              \
	    rthal_irq_descp(irq)->depth = 1;            \
	} while(0);
#define rthal_mark_irq_enabled(irq) do {                 \
	    rthal_irq_descp(irq)->depth = 0;             \
	} while(0);
static inline void fp_init(union fp_state *state)
{
    /* FIXME: This is insufficient. */
    memset(state, 0, sizeof(*state));
}

#endif /* _XENO_ASM_ARM_WRAPPERS_H */
