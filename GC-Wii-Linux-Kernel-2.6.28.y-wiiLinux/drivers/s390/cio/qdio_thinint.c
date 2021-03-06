/*
 * linux/drivers/s390/cio/thinint_qdio.c
 *
 * thin interrupt support for qdio
 *
 * Copyright 2000-2008 IBM Corp.
 * Author(s): Utz Bacher <utz.bacher@de.ibm.com>
 *	      Cornelia Huck <cornelia.huck@de.ibm.com>
 *	      Jan Glauber <jang@linux.vnet.ibm.com>
 */
#include <linux/io.h>
#include <asm/atomic.h>
#include <asm/debug.h>
#include <asm/qdio.h>
#include <asm/airq.h>
#include <asm/isc.h>

#include "cio.h"
#include "ioasm.h"
#include "qdio.h"
#include "qdio_debug.h"
#include "qdio_perf.h"

/*
 * Restriction: only 63 iqdio subchannels would have its own indicator,
 * after that, subsequent subchannels share one indicator
 */
#define TIQDIO_NR_NONSHARED_IND		63
#define TIQDIO_NR_INDICATORS		(TIQDIO_NR_NONSHARED_IND + 1)
#define TIQDIO_SHARED_IND		63

/* list of thin interrupt input queues */
static LIST_HEAD(tiq_list);

/* adapter local summary indicator */
static unsigned char *tiqdio_alsi;

/* device state change indicators */
struct indicator_t {
	u32 ind;	/* u32 because of compare-and-swap performance */
	atomic_t count; /* use count, 0 or 1 for non-shared indicators */
};
static struct indicator_t *q_indicators;

static void tiqdio_tasklet_fn(unsigned long data);
static DECLARE_TASKLET(tiqdio_tasklet, tiqdio_tasklet_fn, 0);

static int css_qdio_omit_svs;

static inline unsigned long do_clear_global_summary(void)
{
	register unsigned long __fn asm("1") = 3;
	register unsigned long __tmp asm("2");
	register unsigned long __time asm("3");

	asm volatile(
		"	.insn	rre,0xb2650000,2,0"
		: "+d" (__fn), "=d" (__tmp), "=d" (__time));
	return __time;
}

/* returns addr for the device state change indicator */
static u32 *get_indicator(void)
{
	int i;

	for (i = 0; i < TIQDIO_NR_NONSHARED_IND; i++)
		if (!atomic_read(&q_indicators[i].count)) {
			atomic_set(&q_indicators[i].count, 1);
			return &q_indicators[i].ind;
		}

	/* use the shared indicator */
	atomic_inc(&q_indicators[TIQDIO_SHARED_IND].count);
	return &q_indicators[TIQDIO_SHARED_IND].ind;
}

static void put_indicator(u32 *addr)
{
	int i;

	if (!addr)
		return;
	i = ((unsigned long)addr - (unsigned long)q_indicators) /
		sizeof(struct indicator_t);
	atomic_dec(&q_indicators[i].count);
}

void tiqdio_add_input_queues(struct qdio_irq *irq_ptr)
{
	struct qdio_q *q;
	int i;

	/* No TDD facility? If we must use SIGA-s we can also omit SVS. */
	if (!css_qdio_omit_svs && irq_ptr->siga_flag.sync)
		css_qdio_omit_svs = 1;

	for_each_input_queue(irq_ptr, q, i) {
		list_add_rcu(&q->entry, &tiq_list);
		synchronize_rcu();
	}
	xchg(irq_ptr->dsci, 1);
	tasklet_schedule(&tiqdio_tasklet);
}

/*
 * we cannot stop the tiqdio tasklet here since it is for all
 * thinint qdio devices and it must run as long as there is a
 * thinint device left
 */
void tiqdio_remove_input_queues(struct qdio_irq *irq_ptr)
{
	struct qdio_q *q;
	int i;

	for (i = 0; i < irq_ptr->nr_input_qs; i++) {
		q = irq_ptr->input_qs[i];
		/* if establish triggered an error */
		if (!q || !q->entry.prev || !q->entry.next)
			continue;
		list_del_rcu(&q->entry);
		synchronize_rcu();
	}
}

static inline int tiqdio_inbound_q_done(struct qdio_q *q)
{
	unsigned char state;

	if (!atomic_read(&q->nr_buf_used))
		return 1;

	qdio_siga_sync_q(q);
	get_buf_state(q, q->first_to_check, &state);

	if (state == SLSB_P_INPUT_PRIMED)
		/* more work coming */
		return 0;
	return 1;
}

static inline int shared_ind(struct qdio_irq *irq_ptr)
{
	return irq_ptr->dsci == &q_indicators[TIQDIO_SHARED_IND].ind;
}

static void __tiqdio_inbound_processing(struct qdio_q *q)
{
	qdio_perf_stat_inc(&perf_stats.thinint_inbound);
	qdio_sync_after_thinint(q);

	/*
	 * Maybe we have work on our outbound queues... at least
	 * we have to check the PCI capable queues.
	 */
	qdio_check_outbound_after_thinint(q);

again:
	if (!qdio_inbound_q_moved(q))
		return;

	qdio_kick_inbound_handler(q);

	if (!tiqdio_inbound_q_done(q)) {
		qdio_perf_stat_inc(&perf_stats.thinint_inbound_loop);
		goto again;
	}

	qdio_stop_polling(q);
	/*
	 * We need to check again to not lose initiative after
	 * resetting the ACK state.
	 */
	if (!tiqdio_inbound_q_done(q)) {
		qdio_perf_stat_inc(&perf_stats.thinint_inbound_loop2);
		goto again;
	}
}

void tiqdio_inbound_processing(unsigned long data)
{
	struct qdio_q *q = (struct qdio_q *)data;

	__tiqdio_inbound_processing(q);
}

/* check for work on all inbound thinint queues */
static void tiqdio_tasklet_fn(unsigned long data)
{
	struct qdio_q *q;

	qdio_perf_stat_inc(&perf_stats.tasklet_thinint);
again:

	/* protect tiq_list entries, only changed in activate or shutdown */
	rcu_read_lock();

	list_for_each_entry_rcu(q, &tiq_list, entry)
		/* only process queues from changed sets */
		if (*q->irq_ptr->dsci) {

			/* only clear it if the indicator is non-shared */
			if (!shared_ind(q->irq_ptr))
				xchg(q->irq_ptr->dsci, 0);
			/*
			 * don't call inbound processing directly since
			 * that could starve other thinint queues
			 */
			tasklet_schedule(&q->tasklet);
		}

	rcu_read_unlock();

	/*
	 * if we used the shared indicator clear it now after all queues
	 * were processed
	 */
	if (atomic_read(&q_indicators[TIQDIO_SHARED_IND].count)) {
		xchg(&q_indicators[TIQDIO_SHARED_IND].ind, 0);

		/* prevent racing */
		if (*tiqdio_alsi)
			xchg(&q_indicators[TIQDIO_SHARED_IND].ind, 1);
	}

	/* check for more work */
	if (*tiqdio_alsi) {
		xchg(tiqdio_alsi, 0);
		qdio_perf_stat_inc(&perf_stats.tasklet_thinint_loop);
		goto again;
	}
}

/**
 * tiqdio_thinint_handler - thin interrupt handler for qdio
 * @ind: pointer to adapter local summary indicator
 * @drv_data: NULL
 */
static void tiqdio_thinint_handler(void *ind, void *drv_data)
{
	qdio_perf_stat_inc(&perf_stats.thin_int);

	/*
	 * SVS only when needed: issue SVS to benefit from iqdio interrupt
	 * avoidance (SVS clears adapter interrupt suppression overwrite)
	 */
	if (!css_qdio_omit_svs)
		do_clear_global_summary();

	/*
	 * reset local summary indicator (tiqdio_alsi) to stop adapter
	 * interrupts for now, the tasklet will clean all dsci's
	 */
	xchg((u8 *)ind, 0);
	tasklet_hi_schedule(&tiqdio_tasklet);
}

static int set_subchannel_ind(struct qdio_irq *irq_ptr, int reset)
{
	struct scssc_area *scssc_area;
	char dbf_text[15];
	void *ptr;
	int rc;

	scssc_area = (struct scssc_area *)irq_ptr->chsc_page;
	memset(scssc_area, 0, PAGE_SIZE);

	if (reset) {
		scssc_area->summary_indicator_addr = 0;
		scssc_area->subchannel_indicator_addr = 0;
	} else {
		scssc_area->summary_indicator_addr = virt_to_phys(tiqdio_alsi);
		scssc_area->subchannel_indicator_addr =
			virt_to_phys(irq_ptr->dsci);
	}

	scssc_area->request = (struct chsc_header) {
		.length = 0x0fe0,
		.code	= 0x0021,
	};
	scssc_area->operation_code = 0;
	scssc_area->ks = PAGE_DEFAULT_KEY;
	scssc_area->kc = PAGE_DEFAULT_KEY;
	scssc_area->isc = QDIO_AIRQ_ISC;
	scssc_area->schid = irq_ptr->schid;

	/* enable the time delay disablement facility */
	if (css_general_characteristics.aif_tdd)
		scssc_area->word_with_d_bit = 0x10000000;

	rc = chsc(scssc_area);
	if (rc)
		return -EIO;

	rc = chsc_error_from_response(scssc_area->response.code);
	if (rc) {
		sprintf(dbf_text, "sidR%4x", scssc_area->response.code);
		QDIO_DBF_TEXT1(0, trace, dbf_text);
		QDIO_DBF_TEXT1(0, setup, dbf_text);
		ptr = &scssc_area->response;
		QDIO_DBF_HEX2(1, setup, &ptr, QDIO_DBF_SETUP_LEN);
		return rc;
	}

	QDIO_DBF_TEXT2(0, setup, "setscind");
	QDIO_DBF_HEX2(0, setup, &scssc_area->summary_indicator_addr,
		      sizeof(unsigned long));
	QDIO_DBF_HEX2(0, setup, &scssc_area->subchannel_indicator_addr,
		      sizeof(unsigned long));
	return 0;
}

/* allocate non-shared indicators and shared indicator */
int __init tiqdio_allocate_memory(void)
{
	q_indicators = kzalloc(sizeof(struct indicator_t) * TIQDIO_NR_INDICATORS,
			     GFP_KERNEL);
	if (!q_indicators)
		return -ENOMEM;
	return 0;
}

void tiqdio_free_memory(void)
{
	kfree(q_indicators);
}

int __init tiqdio_register_thinints(void)
{
	char dbf_text[20];

	isc_register(QDIO_AIRQ_ISC);
	tiqdio_alsi = s390_register_adapter_interrupt(&tiqdio_thinint_handler,
						      NULL, QDIO_AIRQ_ISC);
	if (IS_ERR(tiqdio_alsi)) {
		sprintf(dbf_text, "regthn%lx", PTR_ERR(tiqdio_alsi));
		QDIO_DBF_TEXT0(0, setup, dbf_text);
		tiqdio_alsi = NULL;
		isc_unregister(QDIO_AIRQ_ISC);
		return -ENOMEM;
	}
	return 0;
}

int qdio_establish_thinint(struct qdio_irq *irq_ptr)
{
	if (!is_thinint_irq(irq_ptr))
		return 0;

	/* Check for aif time delay disablement. If installed,
	 * omit SVS even under LPAR
	 */
	if (css_general_characteristics.aif_tdd)
		css_qdio_omit_svs = 1;
	return set_subchannel_ind(irq_ptr, 0);
}

void qdio_setup_thinint(struct qdio_irq *irq_ptr)
{
	if (!is_thinint_irq(irq_ptr))
		return;
	irq_ptr->dsci = get_indicator();
	QDIO_DBF_HEX1(0, setup, &irq_ptr->dsci, sizeof(void *));
}

void qdio_shutdown_thinint(struct qdio_irq *irq_ptr)
{
	if (!is_thinint_irq(irq_ptr))
		return;

	/* reset adapter interrupt indicators */
	put_indicator(irq_ptr->dsci);
	set_subchannel_ind(irq_ptr, 1);
}

void __exit tiqdio_unregister_thinints(void)
{
	tasklet_disable(&tiqdio_tasklet);

	if (tiqdio_alsi) {
		s390_unregister_adapter_interrupt(tiqdio_alsi, QDIO_AIRQ_ISC);
		isc_unregister(QDIO_AIRQ_ISC);
	}
}
