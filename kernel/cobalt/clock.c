/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@xenomai.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/**
 * @ingroup posix
 * @defgroup posix_time Clocks and timers services.
 *
 * Clocks and timers services.
 *
 * Xenomai POSIX skin supports two clocks:
 *
 * CLOCK_REALTIME maps to the nucleus system clock, keeping time as the amount
 * of time since the Epoch, with a resolution of one nanosecond.
 *
 * CLOCK_MONOTONIC maps to an architecture-dependent high resolution
 * counter, so is suitable for measuring short time
 * intervals. However, when used for sleeping (with
 * clock_nanosleep()), the CLOCK_MONOTONIC clock has a resolution of
 * one nanosecond, like the CLOCK_REALTIME clock.
 *
 * CLOCK_MONOTONIC_RAW is Linux-specific, and provides monotonic time
 * values from a hardware timer which is not adjusted by NTP. This is
 * strictly equivalent to CLOCK_MONOTONIC with Xenomai, which is not
 * NTP adjusted either.
 *
 * Timer objects may be created with the timer_create() service using
 * either of the two clocks. The resolution of these timers is one
 * nanosecond, as is the case for clock_nanosleep().
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/xsh_chap02_08.html#tag_02_08_05">
 * Specification.</a>
 *
 *@{*/

#include <nucleus/vdso.h>
#include <asm-generic/xenomai/arith.h>
#include <asm-generic/xenomai/system.h>
#include "thread.h"

/**
 * Get the resolution of the specified clock.
 *
 * This service returns, at the address @a res, if it is not @a NULL, the
 * resolution of the clock @a clock_id.
 *
 * For CLOCK_REALTIME, CLOCK_MONOTONIC and CLOCK_MONOTONIC_RAW, this
 * resolution is one nanosecond. No other clock is supported.
 *
 * @param clock_id clock identifier, either CLOCK_REALTIME,
 * CLOCK_MONOTONIC or CLOCK_MONOTONIC_RAW;
 *
 * @param res the address where the resolution of the specified clock will be
 * stored on success.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EINVAL, @a clock_id is invalid;
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/clock_getres.html">
 * Specification.</a>
 *
 */
int clock_getres(clockid_t clock_id, struct timespec *res)
{
	switch (clock_id) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
		if (res)
			ns2ts(res, 1);
		break;
	default:
		thread_set_errno(EINVAL);
		return -1;
	}

	return 0;
}

/**
 * Read the host-synchronised realtime clock.
 *
 * Obtain the current time with NTP corrections from the Linux domain
 *
 * @param tp pointer to a struct timespec
 *
 * @retval 0 on success;
 * @retval -1 if no suitable NTP-corrected clocksource is availabel
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/gettimeofday.html">
 * Specification.</a>
 *
 */
static int do_clock_host_realtime(struct timespec *tp)
{
#ifdef CONFIG_XENO_OPT_HOSTRT
	cycle_t now, base, mask, cycle_delta;
	unsigned long mult, shift, nsec, rem;
	struct xnvdso_hostrt_data *hostrt_data;
	unsigned int seq;

	hostrt_data = get_hostrt_data();
	BUG_ON(!hostrt_data);

	if (unlikely(!hostrt_data->live))
		return -1;

	/*
	 * Note: Disabling HW interrupts around writes to hostrt_data ensures
	 * that a reader (on the Xenomai side) cannot interrupt a writer (on
	 * the Linux kernel side) on the same CPU.  The sequence counter is
	 * required when a reader is interleaved by a writer on a different
	 * CPU. This follows the approach from userland, where tasking the
	 * spinlock is not possible.
	 */
retry:
	seq = xnread_seqcount_begin(&hostrt_data->seqcount);

	now = xnarch_get_cpu_tsc();
	base = hostrt_data->cycle_last;
	mask = hostrt_data->mask;
	mult = hostrt_data->mult;
	shift = hostrt_data->shift;
	tp->tv_sec = hostrt_data->wall_time_sec;
	nsec = hostrt_data->wall_time_nsec;

	if (xnread_seqcount_retry(&hostrt_data->seqcount, seq))
		goto retry;

	/*
	 * At this point, we have a consistent copy of the fundamental
	 * data structure - calculate the interval between the current
	 * and base time stamp cycles, and convert the difference
	 * to nanoseconds.
	 */
	cycle_delta = (now - base) & mask;
	nsec += (cycle_delta * mult) >> shift;

	/* Convert to the desired sec, usec representation */
	tp->tv_sec += xnarch_divrem_billion(nsec, &rem);
	tp->tv_nsec = rem;

	return 0;
#else /* CONFIG_XENO_OPT_HOSTRT */
	return -EINVAL;
#endif
}

/**
 * Read the specified clock.
 *
 * This service returns, at the address @a tp the current value of the clock @a
 * clock_id. If @a clock_id is:
 * - CLOCK_REALTIME, the clock value represents the amount of time since the
 *   Epoch, with a precision of one nanosecond;
 * - CLOCK_MONOTONIC, the clock value is given by an architecture-dependent high
 *   resolution counter, with a precision of one nanosecond.
 * - CLOCK_MONOTONIC_RAW, same as CLOCK_MONOTONIC.
 * - CLOCK_HOST_REALTIME, the clock value as seen by the host, typically
 *   Linux. Resolution and precision depend on the host, but it is guaranteed
 *   that both, host and Xenomai, use the same information.
 *
 * @param clock_id clock identifier, either CLOCK_REALTIME, CLOCK_MONOTONIC,
 *        CLOCK_MONOTONIC_RAW or CLOCK_HOST_REALTIME;
 *
 * @param tp the address where the value of the specified clock will be stored.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EINVAL, @a clock_id is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/clock_gettime.html">
 * Specification.</a>
 *
 */
int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
	xnticks_t cpu_time;

	switch (clock_id) {
	case CLOCK_REALTIME:
		ns2ts(tp, xnclock_read());
		break;

	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
		cpu_time = xnpod_get_cpu_time();
		tp->tv_sec =
		    xnarch_uldivrem(cpu_time, ONE_BILLION, &tp->tv_nsec);
		break;

	case CLOCK_HOST_REALTIME:
		if (do_clock_host_realtime(tp) != 0) {
			thread_set_errno(EINVAL);
			return -1;
		}
		break;

	default:
		thread_set_errno(EINVAL);
		return -1;
	}

	return 0;
}

/**
 * Set the specified clock.
 *
 * This allow setting the CLOCK_REALTIME clock.
 *
 * @param clock_id the id of the clock to be set, only CLOCK_REALTIME is
 * supported.
 *
 * @param tp the address of a struct timespec specifying the new date.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EINVAL, @a clock_id is not CLOCK_REALTIME;
 * - EINVAL, the date specified by @a tp is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/clock_settime.html">
 * Specification.</a>
 *
 */
int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
	xnticks_t now, new_date;
	spl_t s;

	if (clock_id != CLOCK_REALTIME
	    || (unsigned long)tp->tv_nsec >= ONE_BILLION) {
		thread_set_errno(EINVAL);
		return -1;
	}

	new_date = ts2ns(tp);

	xnlock_get_irqsave(&nklock, s);
	now = xnclock_read();
	xnclock_adjust((xnsticks_t) (new_date - now));
	xnlock_put_irqrestore(&nklock, s);

	return 0;
}

/**
 * Sleep some amount of time.
 *
 * This service suspends the calling thread until the wakeup time specified by
 * @a rqtp, or a signal is delivered to the caller. If the flag TIMER_ABSTIME is
 * set in the @a flags argument, the wakeup time is specified as an absolute
 * value of the clock @a clock_id. If the flag TIMER_ABSTIME is not set, the
 * wakeup time is specified as a time interval.
 *
 * If this service is interrupted by a signal, the flag TIMER_ABSTIME is not
 * set, and @a rmtp is not @a NULL, the time remaining until the specified
 * wakeup time is returned at the address @a rmtp.
 *
 * The resolution of this service is one nanosecond.
 *
 * @param clock_id clock identifier, either CLOCK_REALTIME,
 * CLOCK_MONOTONIC or CLOCK_MONOTONIC_RAW.
 *
 * @param flags one of:
 * - 0 meaning that the wakeup time @a rqtp is a time interval;
 * - TIMER_ABSTIME, meaning that the wakeup time is an absolute value of the
 *   clock @a clock_id.
 *
 * @param rqtp address of the wakeup time.
 *
 * @param rmtp address where the remaining time before wakeup will be stored if
 * the service is interrupted by a signal.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - ENOTSUP, the specified clock is unsupported;
 * - EINVAL, the specified wakeup time is invalid;
 * - EINTR, this service was interrupted by a signal.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread,
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/clock_nanosleep.html">
 * Specification.</a>
 *
 */
int clock_nanosleep(clockid_t clock_id,
		    int flags,
		    const struct timespec *rqtp, struct timespec *rmtp)
{
	xnthread_t *cur;
	spl_t s;
	int err = 0;

	if (xnpod_unblockable_p())
		return EPERM;

	if (clock_id != CLOCK_MONOTONIC &&
	    clock_id != CLOCK_MONOTONIC_RAW &&
	    clock_id != CLOCK_REALTIME)
		return ENOTSUP;

	if ((unsigned long)rqtp->tv_nsec >= ONE_BILLION)
		return EINVAL;

	if (flags & ~TIMER_ABSTIME)
		return EINVAL;

	cur = xnpod_current_thread();

	xnlock_get_irqsave(&nklock, s);

	thread_cancellation_point(cur);

	xnpod_suspend_thread(cur, XNDELAY, ts2ns(rqtp) + 1,
			     clock_flag(flags, clock_id), NULL);

	thread_cancellation_point(cur);

	if (xnthread_test_info(cur, XNBREAK)) {

		if (flags == 0 && rmtp) {
			xnticks_t now, expiry;
			xnsticks_t rem;

			now = clock_get_ticks(clock_id);
			expiry = xntimer_get_date(&cur->rtimer);
			xnlock_put_irqrestore(&nklock, s);
			rem = expiry - now;

			ns2ts(rmtp, rem > 0 ? rem : 0);
		} else
			xnlock_put_irqrestore(&nklock, s);

		return EINTR;
	}

	xnlock_put_irqrestore(&nklock, s);

	return err;
}

/**
 * Sleep some amount of time.
 *
 * This service suspends the calling thread until the wakeup time specified by
 * @a rqtp, or a signal is delivered. The wakeup time is specified as a time
 * interval.
 *
 * If this service is interrupted by a signal and @a rmtp is not @a NULL, the
 * time remaining until the specified wakeup time is returned at the address @a
 * rmtp.
 *
 * The resolution of this service is one nanosecond.
 *
 * @param rqtp address of the wakeup time.
 *
 * @param rmtp address where the remaining time before wakeup will be stored if
 * the service is interrupted by a signal.
 *
 * @retval 0 on success;
 * @retval -1 with @a errno set if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the specified wakeup time is invalid;
 * - EINTR, this service was interrupted by a signal.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread,
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/nanosleep.html">
 * Specification.</a>
 *
 */
int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
	int err = clock_nanosleep(CLOCK_REALTIME, 0, rqtp, rmtp);

	if (!err)
		return 0;

	thread_set_errno(err);
	return -1;
}

/*@}*/

EXPORT_SYMBOL_GPL(clock_getres);
EXPORT_SYMBOL_GPL(clock_gettime);
EXPORT_SYMBOL_GPL(clock_settime);
EXPORT_SYMBOL_GPL(clock_nanosleep);
EXPORT_SYMBOL_GPL(nanosleep);