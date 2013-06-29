/*!\file sched.c
 * \author Philippe Gerum
 *
 * Copyright (C) 2008 Philippe Gerum <rpm@xenomai.org>.
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
 *
 * \ingroup sched
 */

#include <cobalt/kernel/pod.h>
#include <cobalt/kernel/thread.h>
#include <cobalt/kernel/timer.h>
#include <cobalt/kernel/intr.h>
#include <cobalt/kernel/heap.h>
#include <cobalt/kernel/shadow.h>
#include <asm/xenomai/arith.h>
#include <asm/xenomai/thread.h>

static struct xnsched_class *xnsched_class_highest;

#define for_each_xnsched_class(p) \
   for (p = xnsched_class_highest; p; p = p->next)

static void xnsched_register_class(struct xnsched_class *sched_class)
{
	sched_class->next = xnsched_class_highest;
	xnsched_class_highest = sched_class;

	/*
	 * Classes shall be registered by increasing priority order,
	 * idle first and up.
	 */
	XENO_BUGON(NUCLEUS, sched_class->next &&
		   sched_class->next->weight > sched_class->weight);

	printk(XENO_INFO "scheduling class %s registered.\n", sched_class->name);
}

void xnsched_register_classes(void)
{
	xnsched_register_class(&xnsched_class_idle);
#ifdef CONFIG_XENO_OPT_SCHED_WEAK
	xnsched_register_class(&xnsched_class_weak);
#endif
#ifdef CONFIG_XENO_OPT_SCHED_TP
	xnsched_register_class(&xnsched_class_tp);
#endif
#ifdef CONFIG_XENO_OPT_SCHED_SPORADIC
	xnsched_register_class(&xnsched_class_sporadic);
#endif
	xnsched_register_class(&xnsched_class_rt);
}

#ifdef CONFIG_XENO_OPT_WATCHDOG

static unsigned long wd_timeout_arg = CONFIG_XENO_OPT_WATCHDOG_TIMEOUT;
module_param_named(watchdog_timeout, wd_timeout_arg, ulong, 0644);
MODULE_PARM_DESC(watchdog_timeout, "Watchdog timeout (s)");

/*!
 * @internal
 * \fn void xnsched_watchdog_handler(struct xntimer *timer)
 * \brief Process watchdog ticks.
 *
 * This internal routine handles incoming watchdog ticks to detect
 * software lockups. It kills any offending thread which is found to
 * monopolize the CPU so as to starve the Linux kernel for too long.
 */

static void xnsched_watchdog_handler(struct xntimer *timer)
{
	struct xnsched *sched = xnpod_current_sched();
	struct xnthread *curr = sched->curr;

	if (likely(xnthread_test_state(curr, XNROOT))) {
		xnsched_reset_watchdog(sched);
		return;
	}

	if (likely(++sched->wdcount < wd_timeout_arg))
		return;

	trace_mark(xn_nucleus, watchdog_signal,
		   "thread %p thread_name %s",
		   curr, xnthread_name(curr));

	if (xnthread_test_state(curr, XNUSER)) {
		printk(XENO_WARN "watchdog triggered -- runaway thread "
		       "'%s' signaled\n", xnthread_name(curr));
		xnshadow_call_mayday(curr, SIGDEBUG_WATCHDOG);
	} else {
		printk(XENO_WARN "watchdog triggered -- runaway thread "
		       "'%s' cancelled\n", xnthread_name(curr));
		xnpod_cancel_thread(curr);
	}

	xnsched_reset_watchdog(sched);
}

#endif /* CONFIG_XENO_OPT_WATCHDOG */

void xnsched_init(struct xnsched *sched, int cpu)
{
	char htimer_name[XNOBJECT_NAME_LEN];
	char root_name[XNOBJECT_NAME_LEN];
	union xnsched_policy_param param;
	struct xnthread_init_attr attr;
	struct xnsched_class *p;

	sched->cpu = cpu;

	for_each_xnsched_class(p) {
		if (p->sched_init)
			p->sched_init(sched);
	}

#ifdef CONFIG_SMP
	sprintf(htimer_name, "[host-timer/%u]", cpu);
	sprintf(root_name, "ROOT/%u", cpu);
#else
	strcpy(htimer_name, "[host-timer]");
	strcpy(root_name, "ROOT");
#endif
	sched->status = 0;
	sched->lflags = 0;
	sched->inesting = 0;
	sched->curr = &sched->rootcb;
	/*
	 * No direct handler here since the host timer processing is
	 * postponed to xnintr_irq_handler(), as part of the interrupt
	 * exit code.
	 */
	xntimer_init(&sched->htimer, NULL);
	xntimer_set_priority(&sched->htimer, XNTIMER_LOPRIO);
	xntimer_set_name(&sched->htimer, htimer_name);
	xntimer_set_sched(&sched->htimer, sched);
	sched->zombie = NULL;
#ifdef CONFIG_SMP
	cpus_clear(sched->resched);
#endif

	attr.flags = XNROOT | XNSTARTED | XNFPU;
	attr.name = root_name;
	attr.personality = &generic_personality;
	param.idle.prio = XNSCHED_IDLE_PRIO;

	xnthread_init(&sched->rootcb, &attr,
		      sched, &xnsched_class_idle, &param);

	sched->rootcb.affinity = cpumask_of_cpu(cpu);
	xnstat_exectime_set_current(sched, &sched->rootcb.stat.account);
#ifdef CONFIG_XENO_HW_FPU
	sched->fpuholder = &sched->rootcb;
#endif /* CONFIG_XENO_HW_FPU */

	xnthread_init_root_tcb(&sched->rootcb);

#ifdef CONFIG_XENO_OPT_WATCHDOG
	xntimer_init_noblock(&sched->wdtimer, xnsched_watchdog_handler);
	xntimer_set_name(&sched->wdtimer, "[watchdog]");
	xntimer_set_priority(&sched->wdtimer, XNTIMER_LOPRIO);
	xntimer_set_sched(&sched->wdtimer, sched);
#endif /* CONFIG_XENO_OPT_WATCHDOG */
	xntimerq_init(&sched->timerqueue);
}

void xnsched_destroy(struct xnsched *sched)
{
	xntimer_destroy(&sched->htimer);
	xntimer_destroy(&sched->rootcb.ptimer);
	xntimer_destroy(&sched->rootcb.rtimer);
	xntimer_destroy(&sched->rootcb.rrbtimer);
#ifdef CONFIG_XENO_OPT_WATCHDOG
	xntimer_destroy(&sched->wdtimer);
#endif /* CONFIG_XENO_OPT_WATCHDOG */
	xntimerq_destroy(&sched->timerqueue);
}

/* Must be called with nklock locked, interrupts off. */
struct xnthread *xnsched_pick_next(struct xnsched *sched)
{
	struct xnsched_class *p __maybe_unused;
	struct xnthread *curr = sched->curr;
	struct xnthread *thread;

	if (!xnthread_test_state(curr, XNTHREAD_BLOCK_BITS | XNZOMBIE)) {
		/*
		 * Do not preempt the current thread if it holds the
		 * scheduler lock.
		 */
		if (xnthread_test_state(curr, XNLOCK)) {
			xnsched_set_self_resched(sched);
			return curr;
		}
		/*
		 * Push the current thread back to the runnable queue
		 * of the scheduling class it belongs to, if not yet
		 * linked to it (XNREADY tells us if it is).
		 */
		if (!xnthread_test_state(curr, XNREADY)) {
			xnsched_requeue(curr);
			xnthread_set_state(curr, XNREADY);
		}
	}

	/*
	 * Find the runnable thread having the highest priority among
	 * all scheduling classes, scanned by decreasing priority.
	 */
#ifdef CONFIG_XENO_OPT_SCHED_CLASSES
	for_each_xnsched_class(p) {
		thread = p->sched_pick(sched);
		if (thread) {
			xnthread_clear_state(thread, XNREADY);
			return thread;
		}
	}

	return NULL; /* Never executed because of the idle class. */
#else /* !CONFIG_XENO_OPT_SCHED_CLASSES */
	thread = __xnsched_rt_pick(sched);
	if (unlikely(thread == NULL))
		thread = &sched->rootcb;

	xnthread_clear_state(thread, XNREADY);

	return thread;
#endif /* CONFIG_XENO_OPT_SCHED_CLASSES */
}

/* Must be called with nklock locked, interrupts off. */
void xnsched_zombie_hooks(struct xnthread *thread)
{
	XENO_BUGON(NUCLEUS, thread->sched->zombie != NULL);

	thread->sched->zombie = thread;

	trace_mark(xn_nucleus, sched_finalize,
		   "thread_out %p thread_out_name %s",
		   thread, xnthread_name(thread));

	xnshadow_unmap(thread);
	xnsched_forget(thread);
}

void __xnsched_finalize_zombie(struct xnsched *sched)
{
	struct xnthread *thread = sched->zombie;

	xnthread_cleanup(thread);

	if (xnthread_test_state(sched->curr, XNROOT))
		xnfreesync();

	sched->zombie = NULL;
}

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH

struct xnsched *xnsched_finish_unlocked_switch(struct xnsched *sched)
{
	struct xnthread *last;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

#ifdef CONFIG_SMP
	/* If current thread migrated while suspended */
	sched = xnpod_current_sched();
#endif /* CONFIG_SMP */

	last = sched->last;
	sched->status &= ~XNINSW;

	/* Detect a thread which called xnpod_migrate_thread */
	if (last->sched != sched) {
		xnsched_putback(last);
		xnthread_clear_state(last, XNMIGRATE);
	}

	if (xnthread_test_state(last, XNZOMBIE)) {
		/*
		 * There are two cases where sched->last has the zombie
		 * bit:
		 * - either it had it before the context switch, the
		 * cleanup has be done and sched->zombie is last;
		 * - or it has been killed while the nklocked was unlocked
		 * during the context switch, in which case we must run the
		 * cleanup code, and we do it now.
		 */
		if (sched->zombie != last)
			xnsched_zombie_hooks(last);
	}

	return sched;
}

#endif /* CONFIG_XENO_HW_UNLOCKED_SWITCH */

/* Must be called with nklock locked, interrupts off. */
void xnsched_putback(struct xnthread *thread)
{
	if (xnthread_test_state(thread, XNREADY))
		xnsched_dequeue(thread);
	else
		xnthread_set_state(thread, XNREADY);

	xnsched_enqueue(thread);
	xnsched_set_resched(thread->sched);
}

/* Must be called with nklock locked, interrupts off. */
int xnsched_set_policy(struct xnthread *thread,
		       struct xnsched_class *sched_class,
		       const union xnsched_policy_param *p)
{
	int ret;

	/*
	 * Declaring a thread to a new scheduling class may fail, so
	 * we do that early, while the thread is still a member of the
	 * previous class. However, this also means that the
	 * declaration callback shall not do anything that might
	 * affect the previous class (such as touching thread->rlink
	 * for instance).
	 */
	if (sched_class != thread->base_class) {
		if (sched_class->sched_declare) {
			ret = sched_class->sched_declare(thread, p);
			if (ret)
				return ret;
		}
		sched_class->nthreads++;
	}

	/*
	 * As a special case, we may be called from xnthread_init()
	 * with no previous scheduling class at all.
	 */
	if (likely(thread->base_class != NULL)) {
		if (xnthread_test_state(thread, XNREADY))
			xnsched_dequeue(thread);

		if (sched_class != thread->base_class)
			xnsched_forget(thread);
	}

	thread->sched_class = sched_class;
	thread->base_class = sched_class;
	xnsched_setparam(thread, p);
	thread->bprio = thread->cprio;
	thread->wprio = thread->cprio + sched_class->weight;

	if (xnthread_test_state(thread, XNREADY))
		xnsched_enqueue(thread);

	if (xnthread_test_state(thread, XNSTARTED))
		xnsched_set_resched(thread->sched);

	return 0;
}
EXPORT_SYMBOL_GPL(xnsched_set_policy);

/* Must be called with nklock locked, interrupts off. */
void xnsched_track_policy(struct xnthread *thread,
			  struct xnthread *target)
{
	union xnsched_policy_param param;

	if (xnthread_test_state(thread, XNREADY))
		xnsched_dequeue(thread);
	/*
	 * Self-targeting means to reset the scheduling policy and
	 * parameters to the base ones. Otherwise, make thread inherit
	 * the scheduling data from target.
	 */
	if (target == thread) {
		thread->sched_class = thread->base_class;
		xnsched_trackprio(thread, NULL);
	} else {
		xnsched_getparam(target, &param);
		thread->sched_class = target->sched_class;
		xnsched_trackprio(thread, &param);
	}

	if (xnthread_test_state(thread, XNREADY))
		xnsched_enqueue(thread);

	xnsched_set_resched(thread->sched);
}

/* Must be called with nklock locked, interrupts off. thread must be
 * runnable. */
void xnsched_migrate(struct xnthread *thread, struct xnsched *sched)
{
	struct xnsched_class *sched_class = thread->sched_class;

	if (xnthread_test_state(thread, XNREADY)) {
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	if (sched_class->sched_migrate)
		sched_class->sched_migrate(thread, sched);
	/*
	 * WARNING: the scheduling class may have just changed as a
	 * result of calling the per-class migration hook.
	 */
	xnsched_set_resched(thread->sched);
	thread->sched = sched;

#ifdef CONFIG_XENO_HW_UNLOCKED_SWITCH
	/*
	 * Mark the thread in flight, xnsched_finish_unlocked_switch()
	 * will put the thread on the remote runqueue.
	 */
	xnthread_set_state(thread, XNMIGRATE);
#else /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
	/* Move thread to the remote runnable queue. */
	xnsched_putback(thread);
#endif /* !CONFIG_XENO_HW_UNLOCKED_SWITCH */
}

/* Must be called with nklock locked, interrupts off. thread may be
 * blocked. */
void xnsched_migrate_passive(struct xnthread *thread, struct xnsched *sched)
{
	struct xnsched_class *sched_class = thread->sched_class;

	if (xnthread_test_state(thread, XNREADY)) {
		xnsched_dequeue(thread);
		xnthread_clear_state(thread, XNREADY);
	}

	if (sched_class->sched_migrate)
		sched_class->sched_migrate(thread, sched);
	/*
	 * WARNING: the scheduling class may have just changed as a
	 * result of calling the per-class migration hook.
	 */
	xnsched_set_resched(thread->sched);
	thread->sched = sched;

	if (!xnthread_test_state(thread, XNTHREAD_BLOCK_BITS)) {
		xnsched_requeue(thread);
		xnthread_set_state(thread, XNREADY);
	}
}

#ifdef CONFIG_XENO_OPT_SCALABLE_SCHED

void sched_initq(struct xnsched_mlq *q, int loprio, int hiprio)
{
	int prio;

	q->elems = 0;
	q->loprio = loprio;
	q->hiprio = hiprio;
	q->himap = 0;
	memset(&q->lomap, 0, sizeof(q->lomap));

	for (prio = 0; prio < XNSCHED_MLQ_LEVELS; prio++)
		INIT_LIST_HEAD(q->heads + prio);

	XENO_ASSERT(NUCLEUS,
		    hiprio - loprio + 1 < XNSCHED_MLQ_LEVELS,
		    xnpod_fatal("priority range [%d..%d] is beyond multi-level "
				"queue indexing capabilities",
				loprio, hiprio));
}

static inline int indexmlq(struct xnsched_mlq *q, int prio)
{
	XENO_ASSERT(NUCLEUS,
		    prio >= q->loprio && prio <= q->hiprio,
		    xnpod_fatal("priority level %d is out of range ", prio));
	/*
	 * BIG FAT WARNING: We need to rescale the priority level to a
	 * 0-based range. We use ffnz() to scan the bitmap which MUST
	 * be based on a bit scan forward op. Therefore, the lower the
	 * index value, the higher the priority (since least
	 * significant bits will be found first when scanning the
	 * bitmaps).
	 */
	return q->hiprio - prio;
}

static struct list_head *addmlq(struct xnsched_mlq *q, int prio)
{
	struct list_head *head;
	int hi, lo, idx;

	idx = indexmlq(q, prio);
	head = q->heads + idx;
	q->elems++;

	/* New item is not linked yet. */
	if (list_empty(head)) {
		hi = idx / BITS_PER_LONG;
		lo = idx % BITS_PER_LONG;
		q->himap |= (1UL << hi);
		q->lomap[hi] |= (1UL << lo);
	}

	return head;
}

void sched_insertqlf(struct xnsched_mlq *q, struct xnthread *thread)
{
	struct list_head *head = addmlq(q, thread->cprio);
	list_add(&thread->rlink, head);
}

void sched_insertqff(struct xnsched_mlq *q, struct xnthread *thread)
{
	struct list_head *head = addmlq(q, thread->cprio);
	list_add_tail(&thread->rlink, head);
}

static void removemlq(struct xnsched_mlq *q,
		      struct list_head *entry, int idx)
{
	struct list_head *head;
	int hi, lo;

	head = q->heads + idx;
	list_del(entry);
	q->elems--;

	if (list_empty(head)) {
		hi = idx / BITS_PER_LONG;
		lo = idx % BITS_PER_LONG;
		q->lomap[hi] &= ~(1UL << lo);
		if (q->lomap[hi] == 0)
			q->himap &= ~(1UL << hi);
	}
}

void sched_removeq(struct xnsched_mlq *q, struct xnthread *thread)
{
	removemlq(q, &thread->rlink, indexmlq(q, thread->cprio));
}

static inline int ffsmlq(struct xnsched_mlq *q)
{
	int hi = ffnz(q->himap);
	int lo = ffnz(q->lomap[hi]);
	return hi * BITS_PER_LONG + lo;	/* Result is undefined if none set. */
}

struct xnthread *sched_getq(struct xnsched_mlq *q)
{
	struct xnthread *thread;
	struct list_head *head;
	int idx;

	if (q->elems == 0)
		return NULL;

	idx = ffsmlq(q);
	head = q->heads + idx;
	XENO_BUGON(NUCLEUS, list_empty(head));
	thread = list_first_entry(head, struct xnthread, rlink);
	removemlq(q, &thread->rlink, idx);

	return thread;
}

struct xnthread *sched_findq(struct xnsched_mlq *q, int prio)
{
	struct list_head *head;
	int idx;

	idx = indexmlq(q, prio);
	head = q->heads + idx;
	if (list_empty(head))
		return NULL;

	return list_first_entry(head, struct xnthread, rlink);
}

#else /* !CONFIG_XENO_OPT_SCALABLE_SCHED */

struct xnthread *sched_findq(struct list_head *q, int prio)
{
	struct xnthread *thread;

	if (list_empty(q))
		return NULL;

	/* Find thread leading a priority group. */
	list_for_each_entry(thread, q, rlink) {
		if (prio == thread->cprio)
			return thread;
	}

	return NULL;
}

#endif /* !CONFIG_XENO_OPT_SCALABLE_SCHED */

#ifdef CONFIG_XENO_OPT_VFILE

static struct xnvfile_directory sched_vfroot;

struct vfile_schedlist_priv {
	struct xnthread *curr;
	xnticks_t start_time;
};

struct vfile_schedlist_data {
	int cpu;
	pid_t pid;
	char name[XNOBJECT_NAME_LEN];
	char sched_class[XNOBJECT_NAME_LEN];
	int cprio;
	xnticks_t timeout;
	unsigned long state;
};

static struct xnvfile_snapshot_ops vfile_schedlist_ops;

static struct xnvfile_snapshot schedlist_vfile = {
	.privsz = sizeof(struct vfile_schedlist_priv),
	.datasz = sizeof(struct vfile_schedlist_data),
	.tag = &nkpod_struct.threadlist_tag,
	.ops = &vfile_schedlist_ops,
};

static int vfile_schedlist_rewind(struct xnvfile_snapshot_iterator *it)
{
	struct vfile_schedlist_priv *priv = xnvfile_iterator_priv(it);

	/* &nkpod->threadq cannot be empty (root thread(s)). */
	priv->curr = list_first_entry(&nkpod->threadq, struct xnthread, glink);
	priv->start_time = xnclock_read_monotonic();

	return nkpod->nrthreads;
}

static int vfile_schedlist_next(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedlist_priv *priv = xnvfile_iterator_priv(it);
	struct vfile_schedlist_data *p = data;
	xnticks_t timeout, period;
	struct xnthread *thread;

	if (priv->curr == NULL)
		return 0;	/* All done. */

	thread = priv->curr;
	if (list_is_last(&thread->glink, &nkpod->threadq))
		priv->curr = NULL;
	else
		priv->curr = list_next_entry(thread, glink);

	p->cpu = xnsched_cpu(thread->sched);
	p->pid = xnthread_host_pid(thread);
	memcpy(p->name, thread->name, sizeof(p->name));
	p->cprio = thread->cprio;
	p->state = xnthread_state_flags(thread);
	xnobject_copy_name(p->sched_class, thread->sched_class->name);
	period = xnthread_get_period(thread);
	timeout = xnthread_get_timeout(thread, priv->start_time);
	/*
	 * Here we cheat: thread is periodic and the sampling rate may
	 * be high, so it is indeed possible that the next tick date
	 * from the ptimer progresses fast enough while we are busy
	 * collecting output data in this loop, so that next_date -
	 * start_time > period. In such a case, we simply ceil the
	 * value to period to keep the result meaningful, even if not
	 * necessarily accurate. But what does accuracy mean when the
	 * sampling frequency is high, and the way to read it has to
	 * go through the vfile interface anyway?
	 */
	if (period > 0 && period < timeout &&
	    !xntimer_running_p(&thread->rtimer))
		timeout = period;

	p->timeout = timeout;

	return 1;
}

static int vfile_schedlist_show(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedlist_data *p = data;
	char sbuf[64], pbuf[16], tbuf[16];

	if (p == NULL)
		xnvfile_printf(it,
			       "%-3s  %-6s %-5s  %-8s %-8s  %-10s %s\n",
			       "CPU", "PID", "CLASS", "PRI", "TIMEOUT",
			       "STAT", "NAME");
	else {
		snprintf(pbuf, sizeof(pbuf), "%3d", p->cprio);
		xntimer_format_time(p->timeout, tbuf, sizeof(tbuf));
		xnthread_format_status(p->state, sbuf, sizeof(sbuf));

		xnvfile_printf(it,
			       "%3u  %-6d %-5s  %-8s %-8s  %-10s %s\n",
			       p->cpu,
			       p->pid,
			       p->sched_class,
			       pbuf,
			       tbuf,
			       sbuf,
			       p->name);
	}

	return 0;
}

static struct xnvfile_snapshot_ops vfile_schedlist_ops = {
	.rewind = vfile_schedlist_rewind,
	.next = vfile_schedlist_next,
	.show = vfile_schedlist_show,
};

#ifdef CONFIG_XENO_OPT_STATS

struct vfile_schedstat_priv {
	int irq;
	struct xnthread *curr;
	struct xnintr_iterator intr_it;
};

struct vfile_schedstat_data {
	int cpu;
	pid_t pid;
	unsigned long state;
	char name[XNOBJECT_NAME_LEN];
	unsigned long ssw;
	unsigned long csw;
	unsigned long xsc;
	unsigned long pf;
	xnticks_t exectime_period;
	xnticks_t account_period;
	xnticks_t exectime_total;
	struct xnsched_class *sched_class;
	xnticks_t period;
	int cprio;
};

static struct xnvfile_snapshot_ops vfile_schedstat_ops;

static struct xnvfile_snapshot schedstat_vfile = {
	.privsz = sizeof(struct vfile_schedstat_priv),
	.datasz = sizeof(struct vfile_schedstat_data),
	.tag = &nkpod_struct.threadlist_tag,
	.ops = &vfile_schedstat_ops,
};

static int vfile_schedstat_rewind(struct xnvfile_snapshot_iterator *it)
{
	struct vfile_schedstat_priv *priv = xnvfile_iterator_priv(it);
	int irqnr;

	/*
	 * The activity numbers on each valid interrupt descriptor are
	 * grouped under a pseudo-thread.
	 */
	priv->curr = list_first_entry(&nkpod->threadq, struct xnthread, glink);
	priv->irq = 0;
	irqnr = xnintr_query_init(&priv->intr_it) * NR_CPUS;

	return irqnr + nkpod->nrthreads;
}

static int vfile_schedstat_next(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedstat_priv *priv = xnvfile_iterator_priv(it);
	struct vfile_schedstat_data *p = data;
	struct xnthread *thread;
	struct xnsched *sched;
	xnticks_t period;
	int ret;

	if (priv->curr == NULL)
		/*
		 * We are done with actual threads, scan interrupt
		 * descriptors.
		 */
		goto scan_irqs;

	thread = priv->curr;
	if (list_is_last(&thread->glink, &nkpod->threadq))
		priv->curr = NULL;
	else
		priv->curr = list_next_entry(thread, glink);

	sched = thread->sched;
	p->cpu = xnsched_cpu(sched);
	p->pid = xnthread_host_pid(thread);
	memcpy(p->name, thread->name, sizeof(p->name));
	p->state = xnthread_state_flags(thread);
	p->ssw = xnstat_counter_get(&thread->stat.ssw);
	p->csw = xnstat_counter_get(&thread->stat.csw);
	p->xsc = xnstat_counter_get(&thread->stat.xsc);
	p->pf = xnstat_counter_get(&thread->stat.pf);
	p->sched_class = thread->sched_class;
	p->cprio = thread->cprio;
	p->period = xnthread_get_period(thread);

	period = sched->last_account_switch - thread->stat.lastperiod.start;
	if (period == 0 && thread == sched->curr) {
		p->exectime_period = 1;
		p->account_period = 1;
	} else {
		p->exectime_period = thread->stat.account.total -
			thread->stat.lastperiod.total;
		p->account_period = period;
	}
	p->exectime_total = thread->stat.account.total;
	thread->stat.lastperiod.total = thread->stat.account.total;
	thread->stat.lastperiod.start = sched->last_account_switch;

	return 1;

scan_irqs:
	if (priv->irq >= IPIPE_NR_IRQS)
		return 0;	/* All done. */

	ret = xnintr_query_next(priv->irq, &priv->intr_it, p->name);
	if (ret) {
		if (ret == -EAGAIN)
			xnvfile_touch(it->vfile); /* force rewind. */
		priv->irq++;
		return VFILE_SEQ_SKIP;
	}

	if (!xnarch_cpu_supported(priv->intr_it.cpu))
		return VFILE_SEQ_SKIP;

	p->cpu = priv->intr_it.cpu;
	p->csw = priv->intr_it.hits;
	p->exectime_period = priv->intr_it.exectime_period;
	p->account_period = priv->intr_it.account_period;
	p->exectime_total = priv->intr_it.exectime_total;
	p->pid = 0;
	p->state =  0;
	p->ssw = 0;
	p->xsc = 0;
	p->pf = 0;
	p->sched_class = &xnsched_class_idle;
	p->cprio = 0;
	p->period = 0;

	return 1;
}

static int vfile_schedstat_show(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedstat_data *p = data;
	int usage = 0;

	if (p == NULL)
		xnvfile_printf(it,
			       "%-3s  %-6s %-10s %-10s %-10s %-4s  %-8s  %5s"
			       "  %s\n",
			       "CPU", "PID", "MSW", "CSW", "XSC", "PF", "STAT", "%CPU",
			       "NAME");
	else {
		if (p->account_period) {
			while (p->account_period > 0xffffffffUL) {
				p->exectime_period >>= 16;
				p->account_period >>= 16;
			}
			usage = xnarch_ulldiv(p->exectime_period * 1000LL +
					      (p->account_period >> 1),
					      p->account_period, NULL);
		}
		xnvfile_printf(it,
			       "%3u  %-6d %-10lu %-10lu %-10lu %-4lu  %.8lx  %3u.%u"
			       "  %s\n",
			       p->cpu, p->pid, p->ssw, p->csw, p->xsc, p->pf, p->state,
			       usage / 10, usage % 10, p->name);
	}

	return 0;
}

static int vfile_schedacct_show(struct xnvfile_snapshot_iterator *it,
				void *data)
{
	struct vfile_schedstat_data *p = data;

	if (p == NULL)
		return 0;

	xnvfile_printf(it, "%u %d %lu %lu %lu %lu %.8lx %Lu %Lu %Lu %s %s %d %Lu\n",
		       p->cpu, p->pid, p->ssw, p->csw, p->xsc, p->pf, p->state,
		       xnarch_tsc_to_ns(p->account_period),
		       xnarch_tsc_to_ns(p->exectime_period),
		       xnarch_tsc_to_ns(p->exectime_total),
		       p->name,
		       p->sched_class->name,
		       p->cprio,
		       p->period);

	return 0;
}

static struct xnvfile_snapshot_ops vfile_schedstat_ops = {
	.rewind = vfile_schedstat_rewind,
	.next = vfile_schedstat_next,
	.show = vfile_schedstat_show,
};

/*
 * An accounting vfile is a thread statistics vfile in disguise with a
 * different output format, which is parser-friendly.
 */
static struct xnvfile_snapshot_ops vfile_schedacct_ops;

static struct xnvfile_snapshot schedacct_vfile = {
	.privsz = sizeof(struct vfile_schedstat_priv),
	.datasz = sizeof(struct vfile_schedstat_data),
	.tag = &nkpod_struct.threadlist_tag,
	.ops = &vfile_schedacct_ops,
};

static struct xnvfile_snapshot_ops vfile_schedacct_ops = {
	.rewind = vfile_schedstat_rewind,
	.next = vfile_schedstat_next,
	.show = vfile_schedacct_show,
};

#endif /* CONFIG_XENO_OPT_STATS */

int xnsched_init_proc(void)
{
	struct xnsched_class *p;
	int ret;

	ret = xnvfile_init_dir("sched", &sched_vfroot, &nkvfroot);
	if (ret)
		return ret;

	ret = xnvfile_init_snapshot("threads", &schedlist_vfile, &sched_vfroot);
	if (ret)
		return ret;

	for_each_xnsched_class(p) {
		if (p->sched_init_vfile) {
			ret = p->sched_init_vfile(p, &sched_vfroot);
			if (ret)
				return ret;
		}
	}

#ifdef CONFIG_XENO_OPT_STATS
	ret = xnvfile_init_snapshot("stat", &schedstat_vfile, &sched_vfroot);
	if (ret)
		return ret;
	ret = xnvfile_init_snapshot("acct", &schedacct_vfile, &sched_vfroot);
	if (ret)
		return ret;
#endif /* CONFIG_XENO_OPT_STATS */

	return 0;
}

void xnsched_cleanup_proc(void)
{
	struct xnsched_class *p;

	for_each_xnsched_class(p) {
		if (p->sched_cleanup_vfile)
			p->sched_cleanup_vfile(p);
	}

#ifdef CONFIG_XENO_OPT_STATS
	xnvfile_destroy_snapshot(&schedacct_vfile);
	xnvfile_destroy_snapshot(&schedstat_vfile);
#endif /* CONFIG_XENO_OPT_STATS */
	xnvfile_destroy_snapshot(&schedlist_vfile);
	xnvfile_destroy_dir(&sched_vfroot);
}

#endif /* CONFIG_XENO_OPT_VFILE */