#ifndef __ASM_SH_TIMER_H
#define __ASM_SH_TIMER_H

#include <linux/sysdev.h>
#include <asm/cpu/timer.h>

struct sys_timer_ops {
	int (*init)(void);
	int (*start)(void);
	int (*stop)(void);
#ifndef CONFIG_GENERIC_TIME
	unsigned long (*get_offset)(void);
#endif
};

struct sys_timer {
	const char		*name;

	struct sys_device	dev;
	struct sys_timer_ops	*ops;
};

#define TICK_SIZE (tick_nsec / 1000)

extern struct sys_timer tmu_timer;
extern struct sys_timer *sys_timer;

#ifndef CONFIG_GENERIC_TIME
static inline unsigned long get_timer_offset(void)
{
	return sys_timer->ops->get_offset();
}
#endif

/* arch/sh/kernel/timers/timer.c */
struct sys_timer *get_sys_timer(void);

/* arch/sh/kernel/time.c */
void handle_timer_tick(void);

#endif /* __ASM_SH_TIMER_H */
