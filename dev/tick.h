#ifndef TICK_H
#define TICK_H
/*
 * tick.h -- drop-in profiler of user-defined events and actions
 *
 * Description {{{
 * ---------------
 *
 * The purpose of tick is to let you quickly and easily measure time
 * between events during program execution.  You can start using it
 * by simply #include:ing tick.h in any number of files that need
 * measurement, be it part of your program or a library.  There are
 * no compile-time or run-time requirements, it doesn't need additional
 * libraries and there is no need to alter your build infrastructure
 * in any way to use tick.
 *
 * You use tick by calling macros, which when executed signify an event.
 * It is up to you what the event actually is.  You can describe events
 * via printf() syntax, so you can also add event parameters.  The event
 * is logged and its time is remembered.  tick will also print the time
 * delta since eariler events, which is how you can learn how long did
 * something take.  You can also learn the time since the first event,
 * so you can maintain a comprehensive timeline.
 *
 * Events in tick form a hierarchy, which you can fill level by level.
 * This is used to represent actions (which are delimited by a starting
 * and an ending event), subactions and subevents (actions and events
 * happening during an action).  When you start an action, like a
 * function call, all subsequent events are automatically promoted to
 * the next level.  Each level has its own timer, which allows you to
 * learn the time spent on individual levels, ie. in function calls.
 *
 * In addition events may belong to a thread of your choice.  Threads
 * are not hierarchic, you can designate any thread for events on any
 * level.  When the event is logged tick will also print the delta
 * since the last event in the thread.  This is useful for example
 * in measuring time across invocation of callbacks.
 *
 * All macros accept printf()-style event description in place of '...'.
 * You may also leave it empty, in which case an appropriate default is
 * taken.  You can access all functionality through two kinds of macros,
 * the *_THR() variant allowing you to specify the event's thread,
 * a positive integer.  As a special case $thread == 0 means no thread
 * association.
 *
 * tick is not multithreading-safe.  Period.
 * }}}
 *
 * Sample output {{{
 * -----------------
 *
 *       time since the first logged event
 *       ---+-----------------------------
 *          |
 *          |   level               event description
 *          |   --+--               --------------------.
 *          |     |                                     |
 *          |     |  time since the previous event      |
 *          |     |  ------+----------------------      |
 *          |     |        |                            |
 *          |     |        |     function name          |
 *          |     |        |     and line number        |
 *          |     |        |     ----+----------        |
 *          |     |        |         |                  |
 *          v     v        v         v                  v
 * tick: 0.000020[0] (+0.000003) server:581: parse the command line
 * tick: 0.000023[0] (+0.000003) server:582: read the configuration file
 * tick: 0.000026[0] (+0.000003) server:583: initialize the state
 * tick: 0.000029[0] (+0.000003) server:584: open network sockets
 * tick: 0.000032[0] (+0.000003) server:585: enter the main loop
 * tick: 0.000035[0] (+0.000003) serve_client:571: ENTER
 * tick: 0.000038[1] (+0.000003) parse_request:560: parse the request
 * tick: 0.000041[2] (+0.000003) parse_request:561: parse xml
 * tick: 0.000047[2] (+0.000006) node_cb:546: TICK (thread1: start)
 * tick: 0.000050[2] (+0.000003) text_cb:541: TICK (thread2: start)
 * tick: 0.000053[2] (+0.000003) node_cb:546: TICK (thread1: +0.000006)
 * tick: 0.000057[2] (+0.000004) text_cb:541: TICK (thread2: +0.000007)
 * tick: 0.000061[2] (+0.000004) node_cb:546: TICK (thread1: +0.000008)
 * tick: 0.000064[2] (+0.000003) parse_request:565: internalize
 * tick: 0.000067[1] (+0.000003) parse_request:566: LEAVE (elapsed=0.000029)
 * tick: 0.000071[1] (+0.000004) serve_client:573: run the command
 * tick: 0.000074[1] (+0.000003) serve_client:574: update stats in the database (thread3: start)
 * tick: 0.000077[1] (+0.000003) serve_client:575: send the reply
 * tick: 0.000080[0] (+0.000003) serve_client:576: LEAVE (elapsed=0.000045)
 * tick: 0.000084[0] (+0.000004) serve_client:571: ENTER
 * tick: 0.000087[1] (+0.000003) parse_request:560: parse the request
 * tick: 0.000090[2] (+0.000003) parse_request:561: parse xml
 * tick: 0.000095[2] (+0.000005) node_cb:546: TICK (thread1: start)
 * tick: 0.000098[2] (+0.000003) text_cb:541: TICK (thread2: start)
 * tick: 0.000102[2] (+0.000004) node_cb:546: TICK (thread1: +0.000007)
 * tick: 0.000105[2] (+0.000003) text_cb:541: TICK (thread2: +0.000007)
 * tick: 0.000109[2] (+0.000004) node_cb:546: TICK (thread1: +0.000007)
 * tick: 0.000112[2] (+0.000003) parse_request:565: internalize
 * tick: 0.000115[1] (+0.000003) parse_request:566: LEAVE (elapsed=0.000028)
 * tick: 0.000119[1] (+0.000004) serve_client:573: run the command
 * tick: 0.000122[1] (+0.000003) serve_client:574: update stats in the database (thread3: +0.000048)
 * tick: 0.000126[1] (+0.000004) serve_client:575: send the reply
 * tick: 0.000129[0] (+0.000003) serve_client:576: LEAVE (elapsed=0.000045)
 * tick: 0.000132[0] (+0.000003) server:588: bye
 * }}}
 *
 * Synopsis {{{
 * ------------
 *
 * -- TICK(...)
 * -- TICK_THR(thread, ...):
 *    This is the basic interface to log an individual event.
 *
 * -- TICK_ENTER(...)
 * -- TICK_ENTER_THR(thread, ...):
 *    Log an event, possibly the starting of a subroutine, and increase
 *    subsequent events' level by one.
 *
 * -- TICK_LEAVE(...)
 * -- TICK_LEAVE_THR(thread, ...):
 *    As the opposit of TICK_ENTER(), returns from the higher level and
 *    logs the event, possibly the return of a function.  Enters and leaves
 *    should be balanced.
 *
 * -- ENTER_TICK(...)
 * -- ENTER_TICK_THR(thread, ...):
 *    Like TICK_ENTER() but first increases the level then logs the event.
 *    May be convenient at times.
 *
 * -- LEAVE_TICK(...)
 * -- LEAVE_TICK_THR(thread, ...):
 *    Logs the event then returns from the higher level.
 *
 * -- TICK_PEAK(...)
 * -- TICK_PEAK_THR(thread, ...):
 *    Reports the event at the higher level, but doesn't enter it.
 *    May be useful in reducing log clutter.
 *
 * -- TICK_START(...):
 *    Restarts the timeline and all the threads.  If the event description
 *    is omitted doesn't log an event.
 * -- TICK_START_THR(thread, ...):
 *    Restarts $thread's timer, but not the main timeline.  If $thread is ~0,
 *    restarts all thread timers.  Like above no new event is logged if it's
 *    not explicitly described.
 *
 * -- TICKLE(dir, thread, depth, ...):
 *    This is the low-level ninja interface, which lets you specify the
 *    $depth of reporting deltas relative to the last events of lower
 *    levels.  $dir tells how to change the level before/after logging
 *    the event: -2 is used by LEAVE_TICK(), -1: TICK_LEAVE(), 0: TICK(),
 *    +1: TICK_ENTER(), +2: ENTER_TICK(), +3: TICK_PEAK().
 * }}}
 *
 * Compile-time options {{{
 * ------------------------
 *
 * It is possible to control tick via standard definitions:
 * -- TICK_USE_STDERR: If defined, logs go to the standard error.
 *    Otherwise, if <glib.h> has been included before tick.h g_log() is used.
 *    If neither of them is true tick togs to the standard output.
 * -- TICK_DISABLED: Define it and you can continue using TICK*(),
 *    but they won't do anything effectively.
 * }}}
 *
 * Q&A {{{
 * -------
 *
 * Q: It doesn't compile.
 * A: gcc -x c tick.h
 *
 * Q: Can i use it with my zyxyzy licensed software?
 * A: Who cares.
 *
 * Q: Is there a GUI for viewing the results?
 * A: Ja, xterm -e 'less -s output'.
 *
 * Q: Code in header is disgusting, macros are disgusting,
 *    global variables are disgusting.
 * A: So true.
 *
 * Q: oprofile is much better.
 * A: Sure.
 *
 * Q: What does 'tick' mean?  How do you pronounce it?
 * A: This is the sound my officemate makes when illustrating events:
 *    ticktickticktick-brzzzzzzzzzz-ticktickticktick...
 * }}}
 */

/* Public macros {{{ */
#define TICKLE(dir, thread, depth, ...)	\
	tick(0, (dir), (depth), (thread), __FUNCTION__, __LINE__, \
		__VA_ARGS__+0)

#define TICK_THR(thread, ...)		\
	TICKLE(0, (thread), 0, ##__VA_ARGS__)
#define TICK(...)			\
	TICK_THR(0, ##__VA_ARGS__)

#define TICK_ENTER_THR(thread, ...)	\
	TICKLE(1, (thread), 0, ##__VA_ARGS__)
#define TICK_ENTER(...)			\
	TICK_ENTER_THR(0, ##__VA_ARGS__)

#define ENTER_TICK_THR(thread, ...)	\
	TICKLE(2, (thread), 0, ##__VA_ARGS__)
#define ENTER_TICK(...)			\
	ENTER_TICK_THR(0, ##__VA_ARGS__)

#define TICK_LEAVE_THR(thread, ...)	\
	TICKLE(-1, (thread), 0, ##__VA_ARGS__)
#define TICK_LEAVE(...)			\
	TICK_LEAVE_THR(0, ##__VA_ARGS__)

#define LEAVE_TICK_THR(thread, ...)	\
	TICKLE(-2, (thread), 0, ##__VA_ARGS__)
#define LEAVE_TICK(...)			\
	LEAVE_TICK_THR(0, ##__VA_ARGS__)

#define TICK_PEAK_THR(thread, ...)	\
	TICKLE(3, (thread), 0, ##__VA_ARGS__)
#define TICK_PEAK(...)			\
	TICK_PEAK_THR(0, ##__VA_ARGS__)

#define TICK_START_THR(thread, ...)	\
	tick(1, 0, 0, (thread), __FUNCTION__, __LINE__, __VA_ARGS__+0)
#define TICK_START(...)			\
	TICK_START_THR(0, ##__VA_ARGS__)
/* }}} */

#ifdef TICK_DISABLED
  /* disable tick()ing */
# define tick(...)			/* NOP */
#else

/* Include files */
#include <stdlib.h>
#include <stdarg.h>

#include <string.h>
#include <stdio.h>
#include <getopt.h>

#include <sys/time.h>

/* Private macros {{{ */
/* Where to log? */
#if defined(TICK_USE_STDERR)
  /* to stderr */
# define LOGIT(fmt, ...)		\
	fprintf(stderr, "tick: " fmt "\n", ##__VA_ARGS__)
#elif !defined(__G_LIB_H__)
  /* to stdout */
# define LOGIT(fmt, ...)		\
	printf("tick: " fmt "\n", ##__VA_ARGS__)
#else
  /* with glib */
# define LOGIT(fmt, ...)		\
	g_log("tick", G_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#endif

#define TICK_STR_LEN(str)		str, (sizeof(str)-1)
#define TICK_ISSET(tv)			((tv).tv_sec || (tv).tv_usec)
/* }}} */

/* Type definitions {{{ */
/* Describes our state. */
struct tick_st
{
	/*
	 * -- $start:	Time of the first tick() or the last time
	 *		it was restarted.  Used in calculating the
	 *		first column of the output.
	 * -- $last_level:
	 *		Which level was current in the hierarchy of
	 *		timers before the next tick().  Initially 0,
	 *		influenced by tick()'s $dir:ection parameter.
	 */
	struct timeval start;
	unsigned last_level;

	/*
	 * -- $levels:	The hierarchy of timers used to calculate the
	 *		elapsed time between tick()s on the same level.
	 *		Lower levels dominate higher ones.
	 * -- $threads:	Time of the last tick()s happening in threads.
	 */
	unsigned nlevels, nthreads;
	struct timeval *levels, *threads;

	/*
	 * -- $buf:	Used to incrementally build output strings
	 *		via tick_buf_*(), always NUL-terminated.
	 * -- $sbuf:	Allocated size of $buf.
	 * -- $lbuf:	Current number of characters in $buf.
	 */
	char *buf;
	unsigned sbuf, lbuf;
};
/* }}} */

/* Private variables */
/* Points to the global state, shared across all tick instances. */
static struct tick_st *Tick;

/* Program code */
/* Private functions */
/* Utilities {{{ */
/* Ensures that you can access the $nwant:th element of $ptr,
 * an array of $size1 elements.  If $ptr is not large enouth,
 * it $prealloc:ates that many more in advance. */
static void *tick_ensure_alloc(void *ptr, size_t size1,
	unsigned *nnowp, unsigned nwant, unsigned prealloc)
	__attribute__((no_instrument_function));
void *tick_ensure_alloc(void *ptr, size_t size1,
	unsigned *nnowp, unsigned nwant, unsigned prealloc)
{
	if (*nnowp <= nwant)
	{
		nwant += prealloc;
		ptr = realloc(ptr, size1*nwant);
		memset(ptr + size1 * *nnowp, 0, size1 * (nwant-*nnowp));
		*nnowp = nwant;
	}

	return ptr;
} /* tick_ensure_alloc */

/* $now >= $prev => $diff <- $now - $prev */
static void tick_difftime(struct timeval *diff,
	struct timeval const *now, struct timeval const *prev)
	__attribute__((no_instrument_function));
void tick_difftime(struct timeval *diff,
	struct timeval const *now, struct timeval const *prev)
{
	diff->tv_sec = now->tv_sec - prev->tv_sec;
	if (now->tv_usec < prev->tv_usec)
	{
		diff->tv_usec = (1000000 - prev->tv_usec) + now->tv_usec;
		diff->tv_sec--;
	} else
		diff->tv_usec = now->tv_usec - prev->tv_usec;
} /* tick_difftime */
/* }}} */

/* $Tick->buf formatters {{{ */
/* Append a printf() format to $Tick->buf. */
static void tick_buf_vfmt(char const *fmt, va_list printf_args)
	__attribute__((no_instrument_function));
void tick_buf_vfmt(char const *fmt, va_list printf_args)
{
	/* Since we don't know beforehand how large $Tick->buf we need
	 * printf() until it says all characters are written. */
	for (;;)
	{
		int space, len;

		space = Tick->sbuf-Tick->lbuf;
		len = vsnprintf(&Tick->buf[Tick->lbuf], space,
			fmt, printf_args);
		if (space > len)
		{
			Tick->lbuf += len;
			break;
		}

		Tick->buf = (char *)tick_ensure_alloc(
			Tick->buf, sizeof(*Tick->buf),
			&Tick->sbuf, len+1, 32);
	} /* for */
} /* tick_buf_vfmt */

/* Likewise. */
static void tick_buf_fmt(char const *fmt, ...)
	__attribute__((no_instrument_function));
void tick_buf_fmt(char const *fmt, ...)
{
	va_list printf_args;

	va_start(printf_args, fmt);
	tick_buf_vfmt(fmt, printf_args);
	va_end(printf_args);
} /* tick_buf_fmt */

/* Append $str, an $lstr-length string to $Tick->buf. */
static void tick_buf_str(char const *str, size_t lstr)
	__attribute__((no_instrument_function));
void tick_buf_str(char const *str, size_t lstr)
{
	Tick->buf = (char *)tick_ensure_alloc(Tick->buf, sizeof(*Tick->buf),
		&Tick->sbuf, Tick->lbuf + lstr + 1, 32);
	memcpy(&Tick->buf[Tick->lbuf], str, lstr+1);
	Tick->lbuf += lstr;
} /* tick_buf_str */

/* Empty $Tick->buf. */
static void tick_buf_reset(void) __attribute__((no_instrument_function));
void tick_buf_reset(void)
{
	Tick->lbuf = 0;
	Tick->buf[0] = '\0';
} /* tick_buf_reset */

/* Allocate $Tick->buf. */
static void tick_buf_init(void) __attribute__((no_instrument_function));
void tick_buf_init(void)
{
	Tick->buf = (char *)tick_ensure_alloc(
		Tick->buf, sizeof(*Tick->buf), &Tick->sbuf, 128, 0);
} /* tick_buf_init */
/* }}} */

/* Append $depth many more times starting from $level to $Tick->buf.
 * Add the $thread too if there's any. */
static void tick_times(struct timeval const *now,
	unsigned level, unsigned depth, unsigned thread)
	__attribute__((no_instrument_function));
void tick_times(struct timeval const *now,
	unsigned level, unsigned depth, unsigned thread)
{
	struct timeval diff;

	if (!depth && !thread)
		return;

	if (depth)
	{
		unsigned i;

		tick_difftime(&diff, now, &Tick->levels[level]);
		tick_buf_fmt(" (elapsed=%lu.%06lu",
			diff.tv_sec, diff.tv_usec);
		if (depth > level + 1)
			depth = level + 1;
		for (i = 1; i < depth; i++)
		{
			tick_difftime(&diff, now, &Tick->levels[--level]);
			tick_buf_fmt(", %lu.%06lu",
				diff.tv_sec, diff.tv_usec);
		}
	}

	if (thread)
	{
		if (TICK_ISSET(Tick->threads[thread-1]))
		{
			tick_difftime(&diff, now, &Tick->threads[thread-1]);
			tick_buf_fmt(depth
					? ", thread%u: +%lu.%06lu)"
					: " (thread%u: +%lu.%06lu)",
				thread, diff.tv_sec, diff.tv_usec);
		} else
			tick_buf_fmt(depth
					? ", thread%u: start)"
					: " (thread%u: start)",
				thread);
	} else if (depth)
		tick_buf_str(TICK_STR_LEN(")"));
} /* tick_times */

/* Constructors */
/*
 * Either find the common $Tick state or publish ours.  This cooperation
 * is necessary because we can be #include:d in many places and we couldn't
 * rely on the linker to merge common state variables because it only works
 * if the variables are exported, which we cannot take for granted.
 */
static void tick_init(void)
	__attribute__((constructor, no_instrument_function));
void tick_init(void)
{
	/* Using $optarg instead of environment variables as a communication
	 * platform between tick instances prevents problems with programs
	 * re-exec()ing themselves, because the environment is not cleared,
	 * but variables are. */
	if (!optarg || sscanf(optarg, "%p", &Tick) != 1)
	{
		static char state_addr[20];
		static struct tick_st state;

		/* We are the first tick instance to init. */
		Tick = &state;
		sprintf(state_addr, "%p", Tick);
		optarg = state_addr;
		tick_buf_init();
	}
} /* tick_init */

/* Interface functions */
/*
 * Log a tick.
 */
static void tick(int restart, int dir, unsigned depth, unsigned thread,
	char const *fun, unsigned line, char const *fmt, ...)
	__attribute__((unused, format(printf, 7, 8), no_instrument_function));
void tick(int restart, int dir, unsigned depth, unsigned thread,
	char const *fun, unsigned line, char const *fmt, ...)
{
	unsigned level;
	va_list printf_args;
	int preorder, just_peak;
	struct timeval now, startdiff, lastdiff;

	/* Decode $dir and determine the level we're going to.
	 * Make sure we don't underflow. */
	if (dir > 0)
		level = Tick->last_level + 1;
	else if (dir < 0 && Tick->last_level > 0)
		level = Tick->last_level - 1;
	else
		level = Tick->last_level;
	just_peak = dir == 3;
	preorder = -1 <= dir && dir <= 1;

	/* Restart timers. */
	gettimeofday(&now, NULL);
	if (restart)
	{
		switch (thread)
		{
		case  0: /* Restart the timeline. */
			Tick->start = now;
		case ~0: /* Restart all thread timers. */
			memset(Tick->threads, 0,
				sizeof(*Tick->threads) * Tick->nthreads);
			thread = 0;
			break;
		default: /* Restart $thread if it exists. */
			if (thread <= Tick->nthreads)
			{
				Tick->threads[thread-1].tv_sec  = 0;
				Tick->threads[thread-1].tv_usec = 0;
			}
		}
	} /* if */

	/* Allocate structures. */
	Tick->levels = (timeval *)tick_ensure_alloc(Tick->levels,
		sizeof(*Tick->levels), &Tick->nlevels, level, 5);
	if (thread)
		Tick->threads = (timeval *)tick_ensure_alloc(Tick->threads,
			sizeof(*Tick->threads), &Tick->nthreads, thread-1, 5);

	/* Add the user message to $Tick->buf. */
	va_start(printf_args, fmt);
	tick_buf_reset();
	if (fmt)
		tick_buf_vfmt(fmt, printf_args);
	else if (level > Tick->last_level && !just_peak)
		tick_buf_str(TICK_STR_LEN("ENTER"));
	else if (level < Tick->last_level)
		tick_buf_str(TICK_STR_LEN("LEAVE"));
	else if (!restart)
		tick_buf_str(TICK_STR_LEN("TICK"));
	va_end(printf_args);
	if (!fmt && !Tick->lbuf)
		/* Don't log empty restarts. */
		return;

	/* Calculate the rest of the timings and log. */
	if (!TICK_ISSET(Tick->levels[0]))
	{	/* First time we ever tick()ing. */
		/* assert(Tick->last_level == 0) */
		/* assert((!dir && level == 0) || (dir > 0 && level == 1)) */
		/* assert(preorder || dir > 0) */
		Tick->start = now;
		tick_times(&now, 0, 0, thread);
		LOGIT("0.000000[%u] %s:%u: %s",
			preorder ? 0 : 1, fun, line, Tick->buf);
		if (dir > 0)
			Tick->levels[0] = now;
	} else if (level >= Tick->last_level)
	{	/* Jump one level up. */
		/* assert(TICK_ISSET(Tick->levels[Tick->last_level])) */
		tick_difftime(&startdiff, &now, &Tick->start);
		tick_difftime(&lastdiff, &now,
			&Tick->levels[Tick->last_level]);
		tick_times(&now,
			Tick->last_level-1, Tick->last_level > 0 ? depth : 0,
			thread);
		LOGIT("%lu.%06lu[%u] (+%lu.%06lu) %s:%u: %s",
			startdiff.tv_sec, startdiff.tv_usec,
			preorder ? Tick->last_level : level,
			lastdiff.tv_sec, lastdiff.tv_usec,
			fun, line, Tick->buf);
		if (preorder)
			Tick->levels[Tick->last_level] = now;
	} else
	{	/* Return from $Tick->last_level. */
		tick_difftime(&startdiff, &now, &Tick->start);
		tick_difftime(&lastdiff, &now,
			&Tick->levels[Tick->last_level]);
		tick_times(&now, level, preorder ? depth+1 : depth, thread);
		LOGIT("%lu.%06lu[%u] (+%lu.%06lu) %s:%u: %s",
			startdiff.tv_sec, startdiff.tv_usec,
			preorder ? level : Tick->last_level,
			lastdiff.tv_sec, lastdiff.tv_usec,
			fun, line, Tick->buf);
	} /* if */

	/* Store the new state. */
	if (!just_peak)
		Tick->last_level = level;
	if (thread)
		Tick->threads[thread-1] = now;
	Tick->levels[level] = now;
} /* tick */
#endif	/* not disabled */

#ifdef TICK_TESTING /* {{{ */
static void text_cb(void)
{
	TICK_THR(2);
}

static void node_cb(void)
{
	TICK_THR(1);
}

static void parse_xml(void)
{
	node_cb();
	text_cb();
	node_cb();
	text_cb();
	node_cb();
}

static void parse_request(void)
{
	TICK_ENTER("parse the request");
	TICK("parse xml");
	TICK_START_THR(1);
	TICK_START_THR(2);
	parse_xml();
	TICK("internalize");
	TICK_LEAVE();
}

static void serve_client(void)
{
	TICK_ENTER();
	parse_request();
	TICK("run the command");
	TICK_THR(3, "update stats in the database");
	TICK("send the reply");
	TICK_LEAVE();
}

static void server(void)
{
	TICK("parse the command line");
	TICK("read the configuration file");
	TICK("initialize the state");
	TICK("open network sockets");
	TICK("enter the main loop");
	serve_client();
	serve_client();
	TICK("bye");
}

int main(void)
{
	TICK();
	TICK("foo");
	TICK("foo %u bar", 10);
	TICK_ENTER();
	TICK_ENTER();
	TICK_THR(1);
	TICK_LEAVE();
	TICK_LEAVE();
	TICK_ENTER();
	TICK_ENTER();
	TICK_THR(1);
	TICKLE(0, 0, -1);
	TICK_LEAVE();
	TICK_LEAVE();
	TICK_START("RESTART");
	TICK();
	TICK();
	TICK();
	ENTER_TICK();
	TICK();
	TICK_PEAK();
	TICK();
	LEAVE_TICK();

	puts("");
	memset(Tick, 0, sizeof(*Tick));
	tick_buf_init();
	TICK_START();
	TICK();

	puts("");
	memset(Tick, 0, sizeof(*Tick));
	tick_buf_init();
	TICK_ENTER();
	TICK();
	TICK_LEAVE();
	TICK();

	puts("");
	memset(Tick, 0, sizeof(*Tick));
	tick_buf_init();
	ENTER_TICK();
	TICK();
	LEAVE_TICK();
	TICK_ENTER();
	TICK();
	TICK_LEAVE();
	TICK();

	puts("");
	server();

	return 0;
}
#endif /* TICK_TESTING }}} */

/* vim: set foldmethod=marker: */
#endif /* ! TICK_H */
