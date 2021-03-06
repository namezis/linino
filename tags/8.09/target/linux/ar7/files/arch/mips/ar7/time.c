/*
 * Carsten Langgaard, carstenl@mips.com
 * Copyright (C) 1999,2000 MIPS Technologies, Inc.  All rights reserved.
 *
 *  This program is free software; you can distribute it and/or modify it
 *  under the terms of the GNU General Public License (Version 2) as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Setting up the clock on the MIPS boards.
 */

#include <linux/version.h>
#include <asm/time.h>
#include <asm/ar7/ar7.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24) /* TODO remove when 2.6.24 is stable */
void __init plat_timer_setup(struct irqaction *irq)
{
	setup_irq(7, irq);
}

void __init ar7_time_init(void)
#else
void __init plat_time_init(void)
#endif
{
	mips_hpt_frequency = ar7_cpu_freq() / 2;
}
