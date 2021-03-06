/*
 * arch/powerpc/platforms/embedded6xx/wii.c
 *
 * Nintendo Wii board-specific support
 * Copyright (C) 2008 The GameCube Linux Team
 * Copyright (C) 2008 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/seq_file.h>
#include <linux/kexec.h>

#include <asm/io.h>
#include <asm/machdep.h>
#include <asm/prom.h>
#include <asm/time.h>
#include <asm/starlet.h>
#include <asm/udbg.h>

#include "flipper-pic.h"
#include "gcnvi_udbg.h"
#include "usbgecko_udbg.h"


static void wii_restart(char *cmd)
{
	starlet_stm_restart();
	local_irq_disable();
	/* spin until power button pressed */
	for (;;)
		cpu_relax();
}

static void wii_power_off(void)
{
	starlet_stm_power_off();
	local_irq_disable();
	/* spin until power button pressed */
	for (;;)
		cpu_relax();
}

static void wii_halt(void)
{
	wii_restart(NULL);
}

static void wii_show_cpuinfo(struct seq_file *m)
{
	seq_printf(m, "vendor\t\t: IBM\n");
	seq_printf(m, "machine\t\t: Nintendo Wii\n");
}

static void __init wii_setup_arch(void)
{
	ug_udbg_init();
	gcnvi_udbg_init();
}

static void __init wii_init_early(void)
{
}

static int __init wii_probe(void)
{
        unsigned long dt_root;

        dt_root = of_get_flat_dt_root();
        if (!of_flat_dt_is_compatible(dt_root, "nintendo,wii"))
                return 0;

        return 1;
}

#ifdef CONFIG_KEXEC
static void wii_shutdown(void)
{
	/* currently not used */
}

static int wii_kexec_prepare(struct kimage *image)
{
	return 0;
}
#endif /* CONFIG_KEXEC */


define_machine(wii) {
	.name			= "wii",
	.probe			= wii_probe,
	.setup_arch		= wii_setup_arch,
	.init_early		= wii_init_early,
	.show_cpuinfo		= wii_show_cpuinfo,
	.restart		= wii_restart,
	.power_off		= wii_power_off,
	.halt			= wii_halt,
	.init_IRQ		= flipper_pic_probe,
	.get_irq		= flipper_pic_get_irq,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
#ifdef CONFIG_KEXEC
	.machine_shutdown	= wii_shutdown,
	.machine_kexec_prepare	= wii_kexec_prepare,
	.machine_kexec		= default_machine_kexec,
#endif
};

