/*
 * sexycat.c -- iSCSI disk dumper {{{
 *
 * Synopsis:
 *   sexycat [<options>] { [<source>] [<destination>] | [ -x <program> ] }
 *
 * Where <source> can be:
 *   -s <iscsi-url>		Specifies the remote iSCSI source to dump.
 *   -S <file-name>		Upload the local <file-name> to the remote
 *				iSCSI destination.
 *   -S -			Read the standard input.  This is the default
 *				if none of -sS is specified.
 * and <destination> is either:
 *   -d <iscsi-url>		The remote iSCSI destination to write to.
 *   -D <file-name>		Download the remote iSCSI source to this
 *				<file-name>.  Unless -O is in effect the
 *				file won't be overwritten.  If the file is
 *				seekable (ie. it's a regular file) its size
 *				is set to the capacity of the iSCSI source.
 *   -D -			Dump the source iSCSI disk to the standatrd
 *				output.  This is the default is none of -dD
 *				is specified.
 *
 * If not in -x mode,  either the <source> or the <destination> must be
 * an iSCSI target.  If both -sd are specified the source iSCSI disk is
 * directly copied to the destination disk.  Otherwise:
 *
 *   -x <program> [<args>...]	Launch <program> and override its file I/O
 *				syscalls so that they can be used with iSCSI
 *				URLs transparently.  This is only possible
 *				if sexycat is built as part of sexywrap,
 *				and a dual library/executable was created.
 *				In this mode all <options> but -i are ignored.
 *				Example: sexywrap -x cat <iscsi-url> > ide.
 *
 * Possible <options> are:
 *   -i <src-initiator-name>	Log in to the source iSCSI target with this
 *				IQN.  If left unspecified a hardcoded default
 *				is used.  Ignored the the source is a local
 *				file.
 *   -I <dst-initiator-name>	Log in to the destination iSCSI target with
 *				this IQN.  If left unspecified the source's
 *				initiator name is used.  Otherwise if the
 *				argument is an empty string the same hardeced
 *				default is used as for <src-initiator-name>.
 *				Ignored if the destination is a local file.
 *   -N				Don't do any I/O, just connect to the iSCSI
 *				device(s) and log in.
 *   -O				Overwrite the local destination <file-name>.
 *   -v				Be more verbose:
 *				-- at level 1 the capacity of the iSCSI disks
 *				   are printed at the beginning; this is the
 *				   default and it's useful to see that the
 *				   connection(s) to the remote iSCSI disk(s)
 *				   works
 *				-- at level 2 it's printed when a block is
 *				   being re-read or rewritten due to a fault
 *				-- at subsequent levels reading of source
 *				   blocks are printed if progress reporting
 *				   is enabled with -p
 *   -q				Be less verbose.  At verbosity level 0 all
 *				informational output is suppressed.
 *   -p <input-progress>	Print input progress every so often.
 *				The first printout is made when the
 *				<input-progress>:nth block is being
 *				or has been read.  For example -p 123
 *				makes the reading of block #123 logged.
 *				Ignored when a local file is being uploaded.
 *   -P <output-progress>	Likewise for output progress.  The first
 *				printout signifies the writing of the
 *				<output-progress>:nth source block.
 *				Ignored if the destination is a local file.
 *   -m <max-source-reqs>	The maximum number of parallel requests
 *				to the source iSCSI device.  If connection
 *				breaks, this number is reduced by the factor
 *				which can be specified with -R.  Ignored
 *				when the source is a local file, otherwise
 *				the default is 32.
 *   -M <max-dest-reqs>		Same for the maximum number of iSCSI write
 *				requests.  Ignored when the destination is
 *				a local file, otherwise the default is 32.
 *   -b <min-output-batch>	Collect at least this number of input chunks
 *				before writing them out.  Writing of larger
 *				batches can be more efficient.  Only effective
 *				if the destination is a local file, otherwise
 *				the default is 32.
 *   -B <max-output-batch>	Write the output batch if this many input
 *				chunks has been collected.  Only effective
 *				if the destination is a local file, otherwise
 *				the default is 64.
 *   -r <retry-delay>		If reading or writing a chunk is failed,
 *				wait <retry-delay> milliseconds before
 *				retrying.  The default is three seconds.
 *   -R <degradation-percent>	When the connection breaks with an iSCSI
 *				device it's supposed to be caused by the
 *				too high amount of parallel iSCSI requests
 *				(at least this is the case with istgt).
 *				This case the maximimum number of requests
 *				(which can be specified with -mM) is reduced
 *				to this percent.  The value must be between
 *				0..100, and the default is 50%.
 *
 * <iscsi-url> is iscsi://<host>[:<port>]/<target-name>/<LUN>.
 * <host> can either be a hostname or an IPv4 or IPv6 address.
 * <target-name> is the target's IQN.  An example for <iscsi-url> is:
 * iscsi://localhost/iqn.2014-07.net.nsn-net.timmy:omu/1
 * (If you get Connection refused unexpectedly, the reason could be that
 *  libiscsi tries to connect to the localhost's IPv6 address.  To fix it
 *  edit /etc/gai.conf so that it includes "precedence ::ffff:0:0/96 100".)
 *
 * To increase effeciency I/O with iSCSI devices and seekable local files
 * can be done out-of-order, that is, a source block $n may be read/written
 * later than $m even if $n < $m.  Operations are done in chunks, whose size
 * is the same as the source or destination iSCSI device's block size.
 * (This could be improved in the future.)  Requests are sent parallel,
 * with a backoff strategy if the server feels overloaded.
 *
 * TODO query and make use of the server's maximal/optimal chunk size
 * TODO full documentation
 *
 * Dependecies: libiscsi 1.4
 * Compilation: cc -Wall -O2 -s -lrt -liscsi sexycat.c -o sexycat
 *
 * The silly `sexy' name refers to the connection with SCSI, which originally
 * was proposed to be pronounced like that.
 *
 * This program is a very far descendent of iscsi-dd.c from libiscsi-1.4.0,
 * so it can be considered a derivative work, inheriting the licensing terms,
 * thus being covered by the GNU GPL v2.0+.
 * }}}
 */

/* Configuration */
/* For POLLRDHUP */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

/* If we're not built as a part of sexywrap, we're building sexycat. */
#ifndef SEXYWRAP
# define SEXYCAT
#endif

#if defined(SEXYWRAP) && defined(SEXYCAT) && !defined(__PIE__)
# error "You need to build sexywrap+sexycat with -shared -pie -fPIE"
#endif

/* Include files {{{ */
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/uio.h>

#include <linux/limits.h>

#include <iscsi.h>
#include <scsi-lowlevel.h>
/* }}} */

/* Standard definitions {{{ */
/* Defaults */
#define DFLT_INITIAL_MAX_ISCSI_REQS	32
#define DFLT_INITIAL_MAX_OUTPUT_QUEUE	(DFLT_INITIAL_MAX_ISCSI_REQS * 2)
#define DFLT_MIN_OUTPUT_BATCH		(DFLT_INITIAL_MAX_OUTPUT_QUEUE / 2)

#define DFLT_ISCSI_MAXREQS_DEGRADATION	50
#define DFLT_ISCSI_REQUEST_RETRY_PAUSE	(3 * 1000)
/* }}} */

/* Macros {{{ */
/* Return the number of elements in $ary. */
#define MEMBS_OF(ary)			(sizeof(ary) / sizeof((ary)[0]))

/* Shortcuts */
#define LBA_OF(task)			((task)->params.read10.lba)
#define LBA_OF_CHUNK(chunk)		LBA_OF((chunk)->read_task)

/*
 * IS_SEXYWRAP():	return whether we're operating on behalf of sexywrap
 * LOCAL_TO_REMOTE():	return whether we're called by sexywrap::write()
 *			or if we're uploading a local file to an iSCSI device
 * REMOTE_TO_LOCAL():	return whether we're called by sexywrap::read()
 *			or if we're downloading an iSCSI disk to a local file
 * REMOTE_TO_REMOTE():	return whether we're copying an iSCSI disk to another
 *			can't be the case if IS_SEXYWRAP()
 */
#if !defined(SEXYWRAP)
# define IS_SEXYWRAP(input)		0
# define LOCAL_TO_REMOTE(input)		(!(input)->src->iscsi)
# define REMOTE_TO_LOCAL(input)		(!(input)->dst->iscsi)
#elif !defined(SEXYCAT)
# define IS_SEXYWRAP(input)		1
# define LOCAL_TO_REMOTE(input)		(!(input)->src)
# define REMOTE_TO_LOCAL(input)		(!(input)->dst)
#else /* SEXYCAT && SEXYWRAP */
# define IS_SEXYWRAP(input)		(!(input)->src||!(input)->dst)
# define LOCAL_TO_REMOTE(input)		(!(input)->src||!(input)->src->iscsi)
# define REMOTE_TO_LOCAL(input)		(!(input)->dst||!(input)->dst->iscsi)
#endif
#define REMOTE_TO_REMOTE(input)		\
	(!LOCAL_TO_REMOTE(input) && !REMOTE_TO_LOCAL(input))

/* What to do with informational messages. */
#if !defined(SEXYWRAP)
# define info(fmt, ...)			fprintf(Info, fmt "\n", __VA_ARGS__)
#elif !defined(SEXYCAT)
  /* sexywrap doesn't need them. */
# define info(fmt, ...)			/* NOP */
#else /* SEXYCAT && SEXYWRAP */
# define info(fmt, ...)			\
do					\
{					\
	assert(Info != NULL);		\
	fprintf(Info, fmt "\n", __VA_ARGS__);	\
} while (0)
#endif
/* }}} */

/* Type definitions {{{ */
/* Currently we're limited to 2 TiB because we're using read10/write10,
 * but this may change in the future. */
typedef unsigned scsi_block_addr_t;
typedef unsigned scsi_block_count_t;

/* Type of function called back when a chunk has been read or written. */
typedef void (*callback_t)(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);

/* Represents an iSCSI source or destination target. */
struct endpoint_st
{
	union
	{
		char const *fname;

		struct
		{	/* Order is important, $url must be
			 * the first member of the structure. */
			struct iscsi_url *url;

			/* The $initiator name to use for log in to $url. */
			char const *initiator;
		};
	};

	/*
	 * NULL if the target is local.  This case $fname either designates
	 * the input/output file name, or standard input/output if it's NULL.
	 */
	struct iscsi_context *iscsi;

	/* Designates the current maximum number of parallel write requests,
	 * which may be decreased by $Opt_maxreqs_degradation.  Zero in case
	 * of local output. */
	unsigned maxreqs;

	/* The destination's block size if it's an iSCSI device, otherwise
	 * the source device's block size.  In the latter case this tells
	 * the amoount of data in a chunk. */
	unsigned blocksize;

	union
	{
		/* Used for remote targets. */
		struct
		{
			/* The number of blocks of the target. */
			scsi_block_count_t nblocks;

			/* In future these may be used to choose
			 * the optimal transfer amount. */
			//unsigned granuality, optimum;
		};

		/* Used for local destination.  If a file is $seekable,
		 * we'll use pwrite[v]() to write blocks out-of-order.
		 * Otherwise they're written sequentially with write[v](). */
		int seekable;
	};
};

/* Represents a unit of data being read or written in a single request.
 * Currently the chunk size is the same as the source or destination
 * device's block size, but this may change in the future. */
struct input_st;
struct chunk_st
{
	/* If the chunk is unused (not reading or writing), points to the
	 * next chunk in the input_st::unused chain. */
	struct chunk_st *next;

	/* All chunks link to the same input_st. */
	struct input_st *input;

	/* Index of the source block being read or written by this chunk
	 * or zero if the chunk is unused. */
	scsi_block_addr_t srcblock;

	/* If the chunk is failed, the number of milliseconds until retry.
	 * This is recalculated by restart_requests().  Zero for unused
	 * chunks. */
	unsigned time_to_retry;

	/* The data carried by this chunk.  When the source is local,
	 * the input buffer is allocated together with the chunk_st.
	 * $wbuf is used by sexywrapper. */
	union
	{
		void *wbuf;
		struct scsi_task *read_task;
		unsigned char rbuf[0];
	};
};

/* Encapsulates all state information needed for writing.  In theory
 * this struct could be a union at the moment, but this may change
 * in the future. */
struct output_st
{
	union
	{
		/* The number of outstanding write requests.  Zero if the
		 * destination is local. */
		unsigned nreqs;

		/* These are only used for local destination.  This case
		 * the output is done in batches with (p)writev(). */
		struct
		{
			/*
			 * The capacity of $iov and $tasks, thus telling
			 * the maximum number of buffers in the batch.
			 * Initially it's $Opt_max_output_queue, but it
			 * may be increased indefinitely during operation.
			 */
			unsigned max;

			/* The actual number of buffers in the batch. */
			unsigned enqueued;

			/* When a chunk is read it's placed in $tasks.
			 * When the batch is flushed the buffers are
			 * copied to $iov, a preallocated iovec array. */
			struct iovec *iov;
			struct scsi_task **tasks;

			/*
			 * Index of the first unwritten source block.
			 * For example if blocks 0..32 and 64..128 have
			 * been written this field if 33.  It helps
			 * determining when the operation is complete.
			 */
			scsi_block_addr_t top_block;
		};
	};
};

/* The main structure of the program, stringing all structures together. */
struct input_st
{
	/* Number of parallel read requests.  Zero if the input is a
	 * local file. */
	unsigned nreqs;

	/* $top_block is the index of the first unread source block,
	 * and keep reading $until is reached.  Both are zero if the
	 * input is a local file. */
	scsi_block_addr_t top_block, until;

	/* The number of bytes that has been read. */
	off_t nread;

	/*
	 * $unused is a list of preallocated chunks ready for reading.
	 * $nunused is the number of chunks in the list; it's only used
	 * by free_surplus_unused_chunks().
	 *
	 * $failed is a list of chunks whose reading or writing failed
	 * and needs to be retried.  $last_failed points to the last
	 * element of the list; it's only used by restart_requests().
	 */
	unsigned nunused;
	struct chunk_st *unused;
	struct chunk_st *failed, *last_failed;

	/* Links to all other structures. */
	struct output_st *output;
	struct endpoint_st *src, *dst;
};
/* }}} */

/* Function prototypes {{{ */
static void __attribute__((noreturn)) usage(void);
static void warnv(char const *fmt, va_list *args);
static void __attribute__((format(printf, 1, 2)))
	warn(char const *fmt, ...);
static void __attribute__((nonnull(1)))
	warn_errno(char const *op);
static void __attribute__((nonnull(2)))
	warn_iscsi(char const *op, struct iscsi_context *iscsi);
static void __attribute__((noreturn, format(printf, 1, 2)))
	die(char const *fmt, ...);

static void *__attribute__((malloc)) xmalloc(size_t size);
static void xrealloc(void *ptrp, size_t size);
static int xpoll(struct pollfd *pfd, unsigned npolls);
static int xfpoll(struct pollfd *pfd,unsigned npolls, struct input_st *input);
static int xread(int fd, unsigned char *buf, size_t sbuf, size_t *nreadp);
static int xpwritev(int fd, struct iovec *iov, unsigned niov,
	off_t offset, int seek);

static int is_connection_error(
	struct iscsi_context *iscsi, char const *which,
	unsigned revents);
static int is_iscsi_error(
	struct iscsi_context *iscsi, struct scsi_task *task,
	char const *op, int status);
static int run_iscsi_event_loop(struct iscsi_context *iscsi,
	unsigned events);

static void add_output_chunk(struct chunk_st *chunk);
static void add_to_output_iov(struct output_st *output,
	struct scsi_task *task, unsigned niov);
static int process_output_queue(int fd,
	struct endpoint_st const *dst, struct output_st *output,
	int more_to_come);

static void chunk_written(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static void chunk_read(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static int restart_requests(struct input_st *input,
	callback_t read_cb, callback_t write_cb);
static int start_iscsi_read_requests(struct input_st *input,
	callback_t read_cb);

static void free_chunks(struct chunk_st *chunk);
static void free_surplus_unused_chunks(struct input_st *input);
static void reduce_maxreqs(struct endpoint_st *endp, char const *which);
static void return_chunk(struct chunk_st *chunk);
static void take_chunk(struct chunk_st *chunk);
static void chunk_failed(struct chunk_st *chunk);

static void done_input(struct input_st *input);
static int init_input(struct input_st *input, struct output_st *output,
	struct endpoint_st *src, struct endpoint_st *dst);

static void endpoint_connected(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static int connect_endpoint(struct iscsi_context *iscsi,
	struct iscsi_url *url);
static int reconnect_endpoint(struct endpoint_st *endp);

static void destroy_endpoint(struct endpoint_st *endp);
static int stat_endpoint(struct endpoint_st *endp, char const *which);
static int init_endpoint(struct endpoint_st *endp, char const *which,
	char const *url);

static int local_to_remote(struct input_st *input);
static int remote_to_local(struct input_st *input, unsigned output_flags);
static int remote_to_remote(struct input_st *input);
/* }}} */

/* Private variables */
/* User controls {{{ */
/* -vq */
static int Opt_verbosity;

/* -pP */
static unsigned Opt_read_progress, Opt_write_progress;

/* -bB */
static unsigned Opt_min_output_batch = DFLT_MIN_OUTPUT_BATCH;
static unsigned Opt_max_output_queue = DFLT_INITIAL_MAX_OUTPUT_QUEUE;

/* -rR */
static unsigned Opt_request_retry_time  = DFLT_ISCSI_REQUEST_RETRY_PAUSE;
static unsigned Opt_maxreqs_degradation = DFLT_ISCSI_MAXREQS_DEGRADATION;
/* }}} */

/*
 * For diagnostic output.  $Info is the FILE on which informational messages
 * like progress are printed.  $Basename is used in error reporting.  It is
 * set up for sexywrap by default, but if we're sexycat, that's changed in
 * main() right away.
 */
static FILE *Info;
static char const *Basename = "sexywrap";

/* Program code */
void usage(void)
{
	printf("usage: %s [-vq] "
		"[-pP <progress>] "
		"[-mM <max-requests> "
		"[-r <retry-pause>] [-R <request-degradation>] "
		"[-bB <batch-size>] "
		"[-i <initiator>] [-N] "
#ifdef SEXYWRAP
		"[-x <program> [<args>...]] "
#endif
		"[-sS <source>] [-O] [-dD <destination>]\n",
		Basename);
	puts("The source code of this program is available at "
		"https://github.com/enadam/various");
	exit(0);
}

void warnv(char const *fmt, va_list *args)
{
#ifdef SEXYWRAP
	/* It's possible, though unlikely that the program
	 * we're preloaded to cleared $stderr. */
	if (!stderr)
		return;
#endif

	fprintf(stderr, "%s: ", Basename);
	vfprintf(stderr, fmt, *args);
	putc('\n', stderr);
}

void warn(char const *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	warnv(fmt, &args);
	va_end(args);
}

void warn_errno(char const *op)
{
#ifdef SEXYWRAP
	if (!stderr)
		return;
#endif
	fprintf(stderr, "%s: %s: %m\n", Basename, op);
}

void warn_iscsi(char const *op, struct iscsi_context *iscsi)
{
#ifdef SEXYWRAP
	if (!stderr)
		return;
#endif
	if (op)
		fprintf(stderr, "%s: %s: %s\n", Basename, op,
			iscsi_get_error(iscsi));
	else
		fprintf(stderr, "%s: %s\n", Basename,
			iscsi_get_error(iscsi));
}

void die(char const *fmt, ...)
{
	va_list args;

	if (fmt)
	{
		va_start(args, fmt);
		warnv(fmt, &args);
		va_end(args);
	}

	exit(1);
}

void *xmalloc(size_t size)
{
	void *ptr;

	if (!(ptr = malloc(size)))
		die("malloc(%zu): %m", size);

	return ptr;
}

void xrealloc(void *ptrp, size_t size)
{
	void *ptr;

	ptr = *(void **)ptrp;
	if (!(ptr = realloc(ptr, size)))
		die("remalloc(%zu): %m", size);
	*(void **)ptrp = ptr;
}

/* On failure $errno is set. */
int xpoll(struct pollfd *pfd, unsigned npolls)
{
	int ret;

	for (;;)
	{
		if ((ret = poll(pfd, npolls, -1)) > 0)
			return ret;
		if (ret < 0 && errno == EINTR)
			continue;
		if (!ret) /* ??? */
			errno = ENODATA;
		return 0;
	}
}

/* On failure $errno is set. */
int xfpoll(struct pollfd *pfd, unsigned npolls, struct input_st *input)
{
	int ret, eintr;

	/* If we don't have failed chunks we can wait indefinitely
	 * and we won't have to update any chunk_st::time_to_retry:es. */
	if (!input->failed)
		return xpoll(pfd, npolls) ? 1 : -1;

	/* poll() as long as it returns EINTR. */
	do
	{
		unsigned timeout, diff;
		struct timespec from, now;
		struct chunk_st *chunk;
		const unsigned ms_per_sec = 1000;
		const unsigned long ns_per_ms = 1000000;

		/* Measure the time beteen the entry and exit from poll() */
		timeout = input->failed->time_to_retry;
		clock_gettime(CLOCK_MONOTONIC, &from);
		ret = poll(pfd, npolls, timeout);
		eintr = ret < 0 && errno == EINTR;
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* $diff := $now - $from */
		diff = (now.tv_sec - from.tv_sec) * ms_per_sec;
		if (now.tv_nsec < from.tv_nsec)
			diff += ms_per_sec;
		diff += (now.tv_nsec - from.tv_nsec) / ns_per_ms;

		/* Subtract the elapsed time from all failed chunks. */
		for (chunk = input->failed; chunk; chunk = chunk->next)
			if (chunk->time_to_retry > diff)
				chunk->time_to_retry -= diff;
			else
				chunk->time_to_retry = 0;
	} while (ret < 0 && eintr);

	return ret;
}

/* Try to fill $buf with $sbuf bytes from $fd, but no hard promises.
 * On error $errno is set and 0 is returned.  Otherwise 1 is returned
 * and the number of bytes actually read is stored in *$nreadp. */
int xread(int fd, unsigned char *buf, size_t sbuf, size_t *nreadp)
{
	*nreadp = 0;
	while (*nreadp < sbuf)
	{
		ssize_t n;

		n = read(fd, &buf[*nreadp], sbuf - *nreadp);
		if (n > 0)
			*nreadp += n;
		else if (n == 0 || errno == ESPIPE)
			return 1;
		else if (errno != EAGAIN && errno != EINTR
				&& errno != EWOULDBLOCK)
			return 0;
	}

	return 1;
}

/* Writes the full $iov to $fd.  On error 0 is returned and $errno is set. */
int xpwritev(int fd, struct iovec *iov, unsigned niov, off_t offset, int seek)
{
	/* Return right away if there's nothing to write. */
	assert(fd >= 0);
	assert(niov > 0);

	/* Write until all of $iov is written out. */
	for (;;)
	{
		int ret;

		ret = seek
			? niov > 1
				? pwritev(fd, iov, niov, offset)
				: pwrite(fd, iov[0].iov_base, iov[0].iov_len,
					offset)
			: niov > 1
				? writev(fd, iov, niov)
				: write(fd, iov[0].iov_base, iov[0].iov_len);
		if (ret < 0)
		{
			if (errno != EAGAIN && errno != EINTR
					&& errno != EWOULDBLOCK)
				return 0;
			continue;
		}

		if (seek)
			offset += ret;

		/* Skip $iov:s we've just written. */
		while (ret >= iov->iov_len)
		{
			ret -= iov->iov_len;
			iov++;
			niov--;
			if (!niov)
				/* We've written out everything. */
				return 1;
		}

		/* We need to write more. */
		iov->iov_len -= ret;
		iov->iov_base += ret;
	}
}

int is_connection_error(struct iscsi_context *iscsi, char const *which,
	unsigned revents)
{
	int error;
	socklen_t serror;

	if (!(revents & (POLLERR|POLLHUP|POLLRDHUP)))
		return 0;
	if (!which)
		return 1;

	serror = sizeof(error);
	if (!(revents & POLLERR))
		warn("iSCSI %s closed the connection", which);
	else if (!getsockopt(iscsi_get_fd(iscsi), SOL_SOCKET, SO_ERROR,
			&error, &serror) && error)
		warn("iSCSI %s: %s", which, strerror(error));
	else if (revents & (POLLHUP|POLLRDHUP))
		warn("iSCSI %s closed the connection", which);
	else
		warn("iSCSI %s: unknown socket error", which);

	return 1;
}

int is_iscsi_error(struct iscsi_context *iscsi, struct scsi_task *task,
	char const *op, int status)
{
	if (status == SCSI_STATUS_GOOD)
		return 0;
	else if (status == SCSI_STATUS_CHECK_CONDITION)
		warn("%s: sense key:%d ascq:%04x",
			op, task->sense.key, task->sense.ascq);
	else if (status != SCSI_STATUS_CANCELLED)
		warn_iscsi(op, iscsi);
	return 1;
}

int run_iscsi_event_loop(struct iscsi_context *iscsi, unsigned events)
{
	if (iscsi_service(iscsi, events) != 0)
	{
		warn_iscsi(NULL, iscsi);
		return 0;
	} else
		return 1;
}

void add_output_chunk(struct chunk_st *chunk)
{
	unsigned lba, i;
	struct output_st *output = chunk->input->output;

	/* Make room for $chunk in $output->tasks if it's full. */
	if (output->enqueued >= output->max)
	{
		unsigned n;

		/* We've used up all $output->tasks.  Allocate +25% */
		n = output->max + output->max/4;
		xrealloc(&output->tasks, sizeof(*output->tasks) * n);
		xrealloc(&output->iov, sizeof(*output->iov) * n);
		output->max = n;
	}

	/* Find a place for $chunk in $output->tasks in which buffers
	 * are ordered by LBA. */
	assert(output->enqueued < output->max);
	lba = LBA_OF_CHUNK(chunk);
	for (i = output->enqueued; i > 0; i--)
		if (LBA_OF(output->tasks[i-1]) < lba)
			break;

	/* Insert $chunk->read_task into $output->tasks. */
	memmove(&output->tasks[i+1], &output->tasks[i],
		sizeof(*output->tasks) * (output->enqueued - i));
	output->tasks[i] = chunk->read_task;
	chunk->read_task = NULL;
	output->enqueued++;

	/* Return $chunk to the list of unused chunks. */
	return_chunk(chunk);
}

void add_to_output_iov(struct output_st *output,
	struct scsi_task *task, unsigned niov)
{
	assert(niov < output->max);
	output->iov[niov].iov_base = task->datain.data;
	output->iov[niov].iov_len  = task->datain.size;
}

int process_output_queue(int fd,
	struct endpoint_st const *dst, struct output_st *output,
	int more_to_come)
{
	int need_to_seek;
	unsigned niov, ntasks;
	unsigned first, block;
	struct scsi_task **tasks, **from_task, **t;

	/*
	 * $niov	:= the number of buffers in the current batch
	 * $first	:= the first block in the batch; if it's
	 *		   $output->top_block, then the bathc can be flushed
	 *		   without seeking
	 * $block	:= the next block we expect in the batch
	 * $tasks	:= where to take from the next buffer of the batch
	 * $ntasks	:= how many buffers till the end of $output->tasks
	 * $from_task	:= the first buffer in the batch
	 * $need_to_seek := whether the current position of $fd is $first
	 */
	niov = 0;
	need_to_seek = 0;
	ntasks = output->enqueued;
	first = block = output->top_block;
	tasks = from_task = output->tasks;

	/* Send all of $output->enqueued batches. */
	assert(output->max > 0);
	for (;;)
	{
		if (niov >= output->max)
		{	/* $output->iov has reached its maximal capacity,
			 * we need to flush it.  Fall through. */
		} else if (!ntasks)
		{	/* We've run out of output.  Flush or break? */
			if (niov < Opt_min_output_batch && more_to_come)
				/* Too little output to flush and there's
				 * $more_to_come. */
				break;
			/* Fall through. */
		} else if (LBA_OF(*tasks) == block)
		{	/* Found the next buffer in $output->tasks. */
			if (fd >= 0)
				add_to_output_iov(output, *tasks, niov);
			niov++;
			tasks++;
			ntasks--;
			block++;
			continue;
		} else if (niov >= Opt_min_output_batch)
		{	/* The current batch has finished and there's
			 * enough output to flush.  Fall through. */
		} else if (dst->seekable)
		{	/* The current batch is too small to output,
			 * but we can see the next one, because the
			 * output is seekable. */
			/* The next batch starts from *tasks. */
			first = LBA_OF(*tasks);
			block = first + 1;
			from_task = tasks;
			if (fd >= 0)
				add_to_output_iov(output, *tasks, 0);
			tasks++;
			ntasks--;
			niov = 1;

			/* Since there's a gap between the previous and this
			 * new batch we need to seek to $first when flushing
			 * the new batch. */
			need_to_seek = 1;

			/* Go gather buffers. */
			continue;
		} else	/* The batch we could possibly output is too small,
			 * and we can't output the next one, because the
			 * output is not seekable.  Wait for more output. */
			break;

		/* Flush $output->iov. */
		if (!niov)
			/* ...but it's empty. */
			return 0;
		if (fd < 0)
			/* We would have flushed something. */
			return 1;

		/* Write the buffers to $fd. */
		if (!xpwritev(fd, output->iov, niov, dst->blocksize * first,
				need_to_seek))
			die("%s: %m", dst->fname ? dst->fname : "(stdout)");

		/* Delete output buffers. */
		for (t = from_task; t < tasks; t++)
			scsi_free_scsi_task(*t);
		memmove(from_task, tasks,
			sizeof(*tasks) * (tasks - from_task));
		output->enqueued = ntasks;

		/* If we've flushed the first possible batch, update
		 * $output->top_block, which then will point to the start
		 * of the next possible batch. */
		if (output->top_block == first)
			output->top_block = block;
		first = block;

		niov = 0;
		need_to_seek = 0;
	} /* until all batches are output or skipped */

	return 0;
}

void chunk_written(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data)
{
	struct scsi_task *task = command_data;
	struct chunk_st *chunk = private_data;
	struct input_st *input = chunk->input;

	assert(task != NULL);
	assert(chunk != NULL);

	if (!LOCAL_TO_REMOTE(input))
	{
		assert(REMOTE_TO_REMOTE(input));
		assert(chunk->read_task);
	}

	assert(input->output->nreqs > 0);
	input->output->nreqs--;

	if (is_iscsi_error(iscsi, task, "write10", status))
	{
		scsi_free_scsi_task(task);
		chunk_failed(chunk);
		return;
	} else
		scsi_free_scsi_task(task);

	if (Opt_write_progress && !(chunk->srcblock % Opt_write_progress))
		info("source block %u copied", chunk->srcblock);

	chunk->srcblock = 0;
	assert(!chunk->time_to_retry);
	if (REMOTE_TO_REMOTE(input))
	{
		scsi_free_scsi_task(chunk->read_task);
		chunk->read_task = NULL;
	}
	return_chunk(chunk);
}

void chunk_read(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data)
{
	struct scsi_task *task = command_data;
	struct chunk_st *chunk = private_data;
	struct endpoint_st *dst = chunk->input->dst;

	assert(chunk != NULL);
	assert(!LOCAL_TO_REMOTE(chunk->input));
	assert(!chunk->read_task);

	assert(task != NULL);
	assert(LBA_OF(task) == chunk->srcblock);

	assert(chunk->input->nreqs > 0);
	chunk->input->nreqs--;

	if (is_iscsi_error(iscsi, task, "read10", status))
	{
		scsi_free_scsi_task(task);
		chunk_failed(chunk);
		return;
	}

	chunk->input->nread += task->datain.size;
	if (Opt_read_progress && !(chunk->srcblock % Opt_read_progress))
		info("source block %u read", chunk->srcblock);

	chunk->read_task = task;
	assert(!chunk->time_to_retry);
	if (REMOTE_TO_LOCAL(chunk->input))
	{
		add_output_chunk(chunk);
		return;
	}

	/* REMOTE_TO_REMOTE() */
	assert(task->datain.size > 0);
	assert(task->datain.size % dst->blocksize == 0);
	if (!iscsi_write10_task(
		dst->iscsi, dst->url->lun,
		task->datain.data, task->datain.size,
		chunk->srcblock, 0, 0, dst->blocksize,
		chunk_written, chunk))
	{
		warn_iscsi("write10", dst->iscsi);
		die(NULL);
	} else
		chunk->input->output->nreqs++;
}

int restart_requests(struct input_st *input,
	callback_t read_cb, callback_t write_cb)
{
	struct chunk_st *prev, *chunk, *next;
	struct output_st *output = input->output;
	struct endpoint_st *src = input->src;
	struct endpoint_st *dst = input->dst;

	/* Do we have anything to do? */
	if (!(chunk = input->failed))
		return 1;

	/* Can we send any requests at all? */
	if (!(input->nreqs < src->maxreqs || output->nreqs < dst->maxreqs))
		return 1;

	/* We need to know the current time to tell whether a failed request
	 * can be retried. */
	prev = NULL;
	do
	{	/* As long as we have failed requests which have reached
		 * $time_to_retry.  Since the list is ordered, the first time
		 * we meet a $chunk still not ready to retry, we can stop. */
		if (chunk->time_to_retry)
			break;

		/* Reissue the failed request if possible. */
		next = chunk->next;
		if (!LOCAL_TO_REMOTE(input) && !chunk->read_task)
		{	/* Re-read */
			if (!(input->nreqs < src->maxreqs))
			{	/* Max number of reqs reached. */
				prev = chunk;
				continue;
			}

			assert(read_cb != NULL);
			if (Opt_verbosity > 1)
				info("re-reading source block %u",
					chunk->srcblock);
			if (!iscsi_read10_task(
				src->iscsi, src->url->lun,
				chunk->srcblock, src->blocksize,
				src->blocksize, read_cb, chunk))
			{	/* It must be some fatal error, eg. OOM. */
				warn_iscsi("read10", src->iscsi);
				return 0;
			} else
				input->nreqs++;
		} else	/* LOCAL_TO_REMOTE() || $chunk->read_task */
		{	/* Rewrite */
			size_t sbuf;
			unsigned char *buf;

			assert(!REMOTE_TO_LOCAL(input));
			if (!(output->nreqs < dst->maxreqs))
			{	/* Max number of reqs reached. */
				prev = chunk;
				continue;
			}

			if (Opt_verbosity > 1)
				info("rewriting source block %u",
					chunk->srcblock);

			if (IS_SEXYWRAP(input))
			{	/* $buf points to some user buffer. */
				buf  = chunk->wbuf;
				sbuf = dst->blocksize;
			} else if (LOCAL_TO_REMOTE(input))
			{	/* In this mode the buffer comes right after
				 * the struct chunk_st. */
				buf  = chunk->rbuf;
				sbuf = dst->blocksize;
			} else
			{	/* REMOTE_TO_REMOTE() */
				buf  = chunk->read_task->datain.data;
				sbuf = chunk->read_task->datain.size;
			}

			assert(write_cb != NULL);
			if (!iscsi_write10_task(
				dst->iscsi, dst->url->lun,
				buf, sbuf, chunk->srcblock, 0, 0,
				dst->blocksize, write_cb, chunk))
			{	/* Incorrectible error. */
				warn_iscsi("write10", dst->iscsi);
				return 0;
			} else
				output->nreqs++;
		}

		/* Unlink $chunk from the failed chain and update $prev,
		 * $input->failed and $input->last_failed. */
		chunk->next = NULL;
		if (chunk == input->failed)
		{
			assert(!prev);
			input->failed = next;
		} else
		{
			assert(prev != NULL);
			prev->next = next;
		}
		if (chunk == input->last_failed)
			input->last_failed = prev;
	} while ((chunk = next) != NULL);

	return 1;
}

int start_iscsi_read_requests(struct input_st *input, callback_t read_cb)
{
	struct endpoint_st *src = input->src;

	/* Issue new read requests as long as we can. */
	assert(!LOCAL_TO_REMOTE(input));
	while (input->unused
		&& input->nreqs < src->maxreqs
		&& input->top_block < input->until)
	{
		struct chunk_st *chunk;

		chunk = input->unused;
		assert(!chunk->read_task);
		assert(!chunk->time_to_retry);

		if (Opt_verbosity > 2
				&& Opt_read_progress
				&& !(input->top_block % Opt_read_progress))
			info("reading source block %u", input->top_block);

		if (!iscsi_read10_task(
			src->iscsi, src->url->lun,
			input->top_block, src->blocksize,
			src->blocksize, read_cb, chunk))
		{
			warn_iscsi("read10", src->iscsi);
			return 0;
		}
		chunk->srcblock = input->top_block++;

		/* Detach $chunk from $input->unused. */
		input->nreqs++;
		take_chunk(chunk);
	} /* read until there are no $input->unused chunks left */

	return 1;
}

/* Preserves $errno. */
void free_chunks(struct chunk_st *chunk)
{
	int serrno;

	serrno = errno;
	while (chunk)
	{
		struct chunk_st *next;

		next = chunk->next;
		if (REMOTE_TO_REMOTE(chunk->input) && chunk->read_task)
			scsi_free_scsi_task(chunk->read_task);
		free(chunk);
		chunk = next;
	}
	errno = serrno;
}

void free_surplus_unused_chunks(struct input_st *input)
{
	unsigned maxreqs;
	struct chunk_st *chunk;

	/* Free $input->unused until $input->nunused drops to $maxreqs. */
	maxreqs = 0;
	if (input->src)
		maxreqs += input->src->maxreqs;
	if (input->dst)
		maxreqs += input->dst->maxreqs;
	assert(maxreqs >= 1);
	while (input->nunused > maxreqs)
	{
		chunk = input->unused;
		assert(chunk != NULL);
		assert(LOCAL_TO_REMOTE(chunk->input) || !chunk->read_task);
		input->unused = chunk->next;
		free(chunk);
		input->nunused--;
	}
}

void reduce_maxreqs(struct endpoint_st *endp, char const *which)
{
	unsigned maxreqs;

	/* Decrease the maximum number of outstanding requests? */
	if (!Opt_maxreqs_degradation || Opt_maxreqs_degradation == 100)
		return;
	assert(Opt_maxreqs_degradation < 100);

	/* Calculate the new $maxreqs of $endp. */
	maxreqs = endp->maxreqs;
	if (maxreqs <= 1)
		return;
	maxreqs *= Opt_maxreqs_degradation;
	maxreqs /= 100;
	if (!maxreqs)
		maxreqs++;
	else if (maxreqs == endp->maxreqs)
		maxreqs--;
	endp->maxreqs = maxreqs;

	if (which)
		info("%s target: number of maximal "
			"outstanding requests reduced to %u",
			which, endp->maxreqs);
}

void return_chunk(struct chunk_st *chunk)
{
	struct input_st *input = chunk->input;

	chunk->next = input->unused;
	input->unused = chunk;
	input->nunused++;
}

void take_chunk(struct chunk_st *chunk)
{
	assert(chunk->input->nunused > 0);
	chunk->input->nunused--;
	chunk->input->unused = chunk->next;
	chunk->next = NULL;
}

void chunk_failed(struct chunk_st *chunk)
{
	struct input_st *input = chunk->input;

	/* Append $chunk to $input->failed. */
	assert(!chunk->next);

	if (!input->failed)
	{
		assert(!input->last_failed);
		input->failed = chunk;
	} else
	{
		assert(input->last_failed);
		assert(!input->last_failed->next);
		input->last_failed->next = chunk;
	}

	input->last_failed = chunk;
	chunk->time_to_retry = Opt_request_retry_time;
}

/* Preserves $errno. */
void done_input(struct input_st *input)
{
	free_chunks(input->unused);
	free_chunks(input->failed);
	input->unused = input->failed = NULL;
}

/* On error 0 is returned and $errno is set.  Otherwise 1 is returned. */
int init_input(struct input_st *input, struct output_st *output,
	struct endpoint_st *src, struct endpoint_st *dst)
{
	unsigned nchunks;
	size_t inline_buf_size;

	memset(input, 0, sizeof(*input));
	input->src = src;
	input->dst = dst;
	input->output = output;

	/*
	 * If we're copying a local file to a remote target the input buffer
	 * is allocated together with the $chunk, the size of which is the
	 * destination device's blocksize.  In this case we need to discount
	 * the space allocated for the $read_task pointer, because its size
	 * is included in the size of the structure.
	 */
	if (!IS_SEXYWRAP(input) && LOCAL_TO_REMOTE(input))
	{
		assert(dst->blocksize > sizeof(struct scsi_task *));
		inline_buf_size = dst->blocksize - sizeof(struct scsi_task *);
	} else
		inline_buf_size = 0;

	/* Create $input->input chunks. */
	nchunks = 0;
	if (src)
		nchunks += src->maxreqs;
	if (dst)
		nchunks += dst->maxreqs;
	for (; nchunks > 0; nchunks--)
	{
		struct chunk_st *chunk;

		if (!(chunk = malloc(sizeof(*chunk) + inline_buf_size)))
		{
			free_chunks(input->unused);
			return 0;
		} else
			memset(chunk, 0, sizeof(*chunk) + inline_buf_size);

		chunk->input = input;
		return_chunk(chunk);
	} /* until $nchunks unused chunks are created */

	return 1;
}

void endpoint_connected(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data)
{
	int *connected = private_data;
	*connected = status == SCSI_STATUS_GOOD;
}

int connect_endpoint(struct iscsi_context *iscsi, struct iscsi_url *url)
{
	int connected;

	iscsi_set_targetname(iscsi, url->target);
	iscsi_set_session_type(iscsi, ISCSI_SESSION_NORMAL);

	connected = -1;
	if (iscsi_full_connect_async(iscsi, url->portal, url->lun,
		endpoint_connected, &connected) != 0)
	{
		warn_iscsi("connect", iscsi);
		return 0;
	}

	do
	{
		struct pollfd pfd;

		pfd.fd = iscsi_get_fd(iscsi);
		pfd.events = iscsi_which_events(iscsi);
		if (!xpoll(&pfd, MEMBS_OF(&pfd)))
		{
			warn_errno("poll");
			return 0;
		} else if (!run_iscsi_event_loop(iscsi, pfd.revents))
		{	/* run_iscsi_event_loop() has logged the error. */
			return 0;
		} else if (!connected)
		{
			warn("connect: %s: %s: %s",
				url->portal, url->target,
				iscsi_get_error(iscsi));
			return 0;
		}
	} while (connected < 0);

	return 1;
}

int reconnect_endpoint(struct endpoint_st *endp)
{
	iscsi_destroy_context(endp->iscsi);
	if (!(endp->iscsi = iscsi_create_context(endp->initiator)))
	{
		warn_errno("iscsi_create_context()");
		return 0;
	} else
		return connect_endpoint(endp->iscsi, endp->url);
}

void destroy_endpoint(struct endpoint_st *endp)
{
	if (endp->iscsi)
	{
		iscsi_destroy_context(endp->iscsi);
		endp->iscsi = NULL;
	} else	/* This is actually $endp->fname. */
		endp->url = NULL;

	if (endp->url)
	{
		iscsi_destroy_url(endp->url);
		endp->url = NULL;
	}
}

int stat_endpoint(struct endpoint_st *endp, char const *which)
{
	struct scsi_task *task;
	struct scsi_readcapacity10 *cap;
	struct scsi_inquiry_block_limits *__attribute__((unused)) inq;

	/* Get the endpoint's nblocks and blocksize. */
	if (!(task = iscsi_readcapacity10_sync(endp->iscsi, endp->url->lun,
		0, 0)))
	{
		warn_iscsi("readcapacity10", endp->iscsi);
		return 0;
	} else if (task->status != SCSI_STATUS_GOOD
		|| !(cap = scsi_datain_unmarshall(task)))
	{
		warn_errno("readcapacity10");
		return 0;
	}

	/* We need a useful blocksize.  I think 512 byte blocks
	 * should be supported by all SCSI devices. */
	endp->blocksize = cap->block_size;
	if (endp->blocksize < 512)
	{
		if (Opt_verbosity > 0)
			warn("%s target reported blocksize=0, ignored",
				which);
		endp->blocksize = 512;
	}
	endp->nblocks = cap->lba + 1;
	scsi_free_scsi_task(task);

#if 0	/* Unused at the moment. */
	/* Get the optimal transfer amounts. */
	if (!(task = iscsi_inquiry_sync(endp->iscsi, endp->url->lun,
		1, SCSI_INQUIRY_PAGECODE_BLOCK_LIMITS, sizeof(*inq))))
	{
		warn_iscsi("inquiry", endp->iscsi);
		return 0;
	} else if (!(inq = scsi_datain_unmarshall(task)))
	{
		warn_iscsi("inquiry", endp->iscsi);
		scsi_free_scsi_task(task);
		return 0;
	} else
	{	/* Ensure: $blocksize <= $granuality <= $optimum <= $max */
		unsigned max;

		/* $max := max(FLOOR($max_xfer_len), $blocksize) */
		max = inq->max_xfer_len;
		max -= max % endp->blocksize;
		if (!max)
			max = endp->blocksize;

		/* $granuality := max($blocksize*$opt_gran, $blocksize)
		 * $granuality := min($granuality, $max) */
		endp->granuality = endp->blocksize * inq->opt_gran;
		if (!endp->granuality)
			endp->granuality = endp->blocksize;
		else if (max && endp->granuality > max)
			endp->granuality = max;

		if (inq->opt_xfer_len)
		{	/* Make $blocksize <= $optimum <= $max. */
			unsigned rem;

			/* $optimum := FLOOR($opt_xfer_len) */
			rem = inq->opt_xfer_len % endp->blocksize;
			endp->optimum = inq->opt_xfer_len - rem;

			/* min($optimum) <- $blocksize
			 * max($optimum) <- $max */
			if (!endp->optimum)
				endp->optimum = endp->blocksize;
			else if (max && endp->optimum > max)
				endp->optimum = max;

			/* max($granuality) <- $optimum */
			if (endp->granuality > endp->optimum)
				endp->granuality = endp->optimum;
		} else	/* $opt_xfer_len == 0 */
			endp->optimum = endp->granuality;
	}
#endif  /* SCSI_INQUIRY_PAGECODE_BLOCK_LIMITS */

	return 1;
}

int init_endpoint(struct endpoint_st *endp, char const *which,
	char const *url)
{
	/* Create $endp->iscsi and connect to $endp->url. */
	if (!(endp->iscsi = iscsi_create_context(endp->initiator)))
	{
		warn_errno("iscsi_create_context()");
		return 0;
	} else if (!(endp->url = iscsi_parse_full_url(endp->iscsi, url)))
	{
		warn_iscsi(NULL, endp->iscsi);
		destroy_endpoint(endp);
		return 0;
	} else if (!connect_endpoint(endp->iscsi, endp->url)
		|| !stat_endpoint(endp, which))
	{
		destroy_endpoint(endp);
		return 0;
	} else if (Opt_verbosity > 0)
		info("%s target: blocksize=%u, nblocks=%u",
			which, endp->blocksize, endp->nblocks);

	return 1;
}

#ifdef SEXYCAT
int local_to_remote(struct input_st *input)
{
	off_t maxwrite;
	int eof, overflow;
	struct pollfd pfd[2];
	struct endpoint_st *src = input->src;
	struct endpoint_st *dst = input->dst;

	/* Open the input file. */
	if (!src->fname || !strcmp(src->fname, "-"))
	{	/* Input is stdin. */
		src->fname = NULL;
		pfd[0].fd = STDIN_FILENO;
	} else if ((pfd[0].fd = open(src->fname, O_RDONLY)) < 0)
	{
		warn_errno(src->fname);
		return 0;
	}

	eof = overflow = 0;
	pfd[1].fd = iscsi_get_fd(dst->iscsi);
	maxwrite = dst->blocksize * dst->nblocks;
	for (;;)
	{
		int ret;

		if (!restart_requests(input, NULL, chunk_written))
			return 0;
		if (eof && !input->output->nreqs && !input->failed)
			break;

		pfd[0].events = !eof && input->unused ? POLLIN : 0;
		pfd[1].events = iscsi_which_events(dst->iscsi);
		if ((ret = xfpoll(pfd, MEMBS_OF(pfd), input)) < 0)
		{
			warn_errno("poll");
			return 0;
		} else if (!ret)
			continue;

		if (pfd[0].revents & POLLIN)
		{	/* We must have been waiting for POLLIN. */
			size_t n;
			struct chunk_st *chunk;

			assert(input->unused != NULL);
			chunk = input->unused;
			if (!xread(pfd[0].fd,chunk->rbuf,dst->blocksize,&n))
			{
				warn_errno(src->fname
					? src->fname : "(stdin)");
				return 0;
			}

			/* Make sure we're within the target's capacity. */
			if (n > maxwrite)
			{	/* Can't write as much as $n bytes. */
				overflow = 1;
				n = maxwrite;
				if (n < dst->blocksize)
					/* Can't write anything more. */
					n = 0;
			}

			if (n > 0)
			{	/* Remove $chunk from $input->unused. */
				take_chunk(chunk);
				chunk->srcblock = input->top_block++;

				assert(n >= dst->blocksize);
				assert(n <= maxwrite);
				maxwrite -= n;

				if (!iscsi_write10_task(
					dst->iscsi, dst->url->lun,
					chunk->rbuf, dst->blocksize,
					chunk->srcblock, 0, 0,
					dst->blocksize, chunk_written, chunk))
				{
					warn_iscsi("write10", dst->iscsi);
					die(NULL);
				} else
					input->output->nreqs++;
			} else /* We didn't read anything. */
				eof = 1;
		}

		if (pfd[0].revents & (POLLHUP|POLLRDHUP))
			eof = 1;

		if (!is_connection_error(dst->iscsi, "destination",
			pfd[1].revents))
		{
			if (!run_iscsi_event_loop(dst->iscsi, pfd[1].revents))
				return 0;
			free_surplus_unused_chunks(input);
		} else
		{
			if (!reconnect_endpoint(dst))
				return 0;
			reduce_maxreqs(dst, "destination");
			free_surplus_unused_chunks(input);
		}
	} /* until $eof is reached and everything is written out */

	/* Close the input file if we opened it. */
	if (!src->fname)
		close(pfd[0].fd);

	if (overflow)
	{
		warn("only %ld bytes could be written",
			dst->blocksize * dst->nblocks - maxwrite);
		return 0;
	} else
		return 1;
}

int remote_to_local(struct input_st *input, unsigned output_flags)
{
	struct pollfd pfd[2];
	struct endpoint_st *src = input->src;
	struct endpoint_st *dst = input->dst;

	/* Open the output file. */
	output_flags |= O_CREAT | O_WRONLY;
	if (!dst->fname || !strcmp(dst->fname, "-"))
	{	/* Output is stdout. */
		dst->fname = NULL;
		pfd[1].fd = STDOUT_FILENO;
	} else if ((pfd[1].fd = open(dst->fname, output_flags, 0666)) < 0)
	{
		warn_errno(dst->fname);
		return 0;
	}

	/* If the output is $seekable we'll possibly use pwrite().
	 * In this case we need to allocate space for it, otherwise
	 * it will return ESPIPE. */
	dst->seekable = lseek(pfd[1].fd, 0, SEEK_CUR) != (off_t)-1;
	if (dst->seekable && ftruncate(pfd[1].fd,
		src->blocksize * src->nblocks) < 0)
	{
		warn_errno(dst->fname);
		return 0;
	}

	pfd[0].fd = iscsi_get_fd(src->iscsi);
	for (;;)
	{
		int eof, ret;

		if (!restart_requests(input, chunk_read, NULL))
			return 0;
		if (!start_iscsi_read_requests(input, chunk_read))
			return 0;

		eof = !input->nreqs && !input->failed;
		if (eof && !input->output->enqueued)
			break;

		pfd[0].events = iscsi_which_events(src->iscsi);
		pfd[1].events = process_output_queue(-1, dst, input->output,
					!eof)
			? POLLOUT : 0;
		if ((ret = xfpoll(pfd, MEMBS_OF(pfd), input)) < 0)
		{
			warn_errno("poll");
			return 0;
		} else if (!ret)
			continue;

		if (!is_connection_error(src->iscsi, "source",
			pfd[0].revents))
		{
			if (!run_iscsi_event_loop(src->iscsi, pfd[0].revents))
				return 0;
		} else
		{
			if (!reconnect_endpoint(src))
				return 0;
			reduce_maxreqs(src, "source");
			free_surplus_unused_chunks(input);
		}

		if (pfd[1].revents)
		{
			process_output_queue(pfd[1].fd, dst, input->output,
				!eof);
			free_surplus_unused_chunks(input);
		}
	}

	assert(input->top_block == input->until);
	assert(input->nread == (off_t)src->blocksize * src->nblocks);

	/* Close the input file if we opened it. */
	if (!dst->fname)
		close(pfd[0].fd);

	return 1;
}

int remote_to_remote(struct input_st *input)
{
	struct pollfd pfd[2];
	struct endpoint_st *src = input->src;
	struct endpoint_st *dst = input->dst;

	pfd[0].fd = iscsi_get_fd(src->iscsi);
	pfd[1].fd = iscsi_get_fd(dst->iscsi);
	for (;;)
	{
		int ret;

		if (!restart_requests(input, chunk_read, chunk_written))
			return 0;
		if (!start_iscsi_read_requests(input, chunk_read))
			return 0;
		if (!input->nreqs && !input->output->nreqs && !input->failed)
			break;

		pfd[0].events = iscsi_which_events(src->iscsi);
		pfd[1].events = iscsi_which_events(dst->iscsi);
		if ((ret = xfpoll(pfd, MEMBS_OF(pfd), input)) < 0)
		{
			warn_errno("poll");
			return 0;
		} else if (!ret)
			continue;

		if (!is_connection_error(src->iscsi, "source",
			pfd[0].revents))
		{
			if (!run_iscsi_event_loop(src->iscsi, pfd[0].revents))
				return 0;
		} else
		{
			if (!reconnect_endpoint(src))
				return 0;
			reduce_maxreqs(src, "source");
			free_surplus_unused_chunks(input);
		}

		if (!is_connection_error(dst->iscsi, "destination",
			pfd[1].revents))
		{
			if (!run_iscsi_event_loop(dst->iscsi, pfd[1].revents))
				return 0;
			free_surplus_unused_chunks(input);
		} else
		{
			if (!reconnect_endpoint(dst))
				return 0;
			reduce_maxreqs(dst, "destination");
			free_surplus_unused_chunks(input);
		}
	}

	assert(input->top_block == input->until);
	assert(input->nread == (off_t)src->blocksize * src->nblocks);
	return 1;
}

/* The main function */
int main(int argc, char *argv[])
{
	struct
	{
		int is_local;
		union
		{
			char const *url;
			char const *fname;
		};
		struct endpoint_st endp;
	} src, dst;
	int isok, optchar, nop;
	unsigned output_flags;
	struct input_st input;
	struct output_st output;
	char const *optstring;

	/* Initialize diagnostic output. */
	Info = stdout;
	setvbuf(stderr, NULL, _IOLBF, 0);
	if ((Basename = strrchr(argv[0], '/')) != NULL)
		Basename++;
	else
		Basename = argv[0];

	/* Prepare our working area.  All data structures are rooted 
	 * from $input: $input.output, $input.src, $input.dst. */
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	memset(&output, 0, sizeof(output));

	/* Parse the command line */
	nop = 0;
	Opt_verbosity = 1;
	output_flags = O_EXCL;

	/* These defaults are used in --debug mode. */
	src.url = "iscsi://127.0.0.1/iqn.2014-07.net.nsn-net.timmy:try/0";
	dst.url = "iscsi://127.0.0.1/iqn.2014-07.net.nsn-net.timmy:try/1";
	if (argv[1] && !strcmp(argv[1], "--debug"))
	{
		argc--;
		argv++;
	} else
		src.url = dst.url = NULL;

#ifdef SEXYWRAP
# define SEXYWRAP_CMDLINE			"x:"
#else
# define SEXYWRAP_CMDLINE			/* none */
#endif
	optstring = "hvqi:s:S:p:m:I:d:D:P:OM:b:B:r:R:N" SEXYWRAP_CMDLINE ;

	while ((optchar = getopt(argc, argv, optstring)) != EOF)
		switch (optchar)
		{
		case 'v':
			Opt_verbosity++;
			break;
		case 'q':
			Opt_verbosity--;
			break;

		/* Source-related options */
		case 'i':
			src.endp.initiator = optarg;
			break;
		case 's':
			src.is_local = 0;
			src.url = optarg;
			break;
		case 'S':
			src.is_local = 1;
			src.fname = optarg;
			break;
		case 'p':
			Opt_read_progress = atoi(optarg);
			break;
		case 'm':
			src.endp.maxreqs = atoi(optarg);
			break;

		/* Destination-related options */
		case 'I':
			dst.endp.initiator = optarg;
			break;
		case 'd':
			dst.is_local = 0;
			dst.url = optarg;
			break;
		case 'D':
			dst.is_local = 1;
			dst.fname = optarg;
			break;
		case 'O':
			output_flags &= ~O_EXCL;
			output_flags |= O_TRUNC;
			break;
		case 'P':
			Opt_write_progress = atoi(optarg);
			break;
		case 'M':
			dst.endp.maxreqs = atoi(optarg);
			break;

		/* Error recovery */
		case 'r':
			Opt_request_retry_time = atoi(optarg);
			break;
		case 'R':
			Opt_maxreqs_degradation = atoi(optarg);
			if (Opt_maxreqs_degradation > 100)
				die("maximum iSCSI requests "
					"degradation must be under 100%%");
			break;

		/* Output batch controls */
		case 'b':
			Opt_min_output_batch = atoi(optarg);
			break;
		case 'B':
			Opt_max_output_queue = atoi(optarg);
			break;

		/* Mode of operation. */
		case 'N':
			nop = 1;
			break;
#ifdef SEXYWRAP
		case 'x':
		{	/* Execute a program preloaded with us. */
			ssize_t n;
			char *env;
			char path[PATH_MAX];
			char const *preload;

			/* Retrieve our absolute path. */
			n = readlink("/proc/self/exe", path, sizeof(path));
			if (n < 0)
			{
				warn_errno("readlink");
				die(NULL);
			} else if (n >= sizeof(path))
			{
				errno = ENAMETOOLONG;
				warn_errno("readlink");
				die(NULL);
			} else	/* readlink() doesn't terminate the string. */
				path[n] = '\0';

			/* Construct and set a new $LD_PRELOAD. */
			if ((preload = getenv("LD_PRELOAD")) != NULL)
			{
				size_t m;

				m = strlen(preload);
				env = xmalloc(m + 1 + n + 1);
				memcpy(env, preload, m);
				env[m] = ':';
				memcpy(&env[m+1], path, n);
				env[m+1+n] = '\0';
				setenv("LD_PRELOAD", env, 1);
				free(env);
			} else
				setenv("LD_PRELOAD", path, 1);

			/* Set the $initiator for sexywrap. */
			if (src.endp.initiator && !src.endp.initiator[0])
				die("invalid source initiator name");
			if (dst.endp.initiator)
				die("destination initiator name "
					"cannot be specified");
			if (src.endp.initiator)
				setenv("SEXYWRAP_INITIATOR",
					src.endp.initiator, 1);

			/* Execute the program. */
			argv += optind - 1;
			execvp(optarg, argv);
			warn_errno(optarg);
			die(NULL);
		} /* -x */
#endif /* SEXYWRAP */

		case 'h':
			usage();
			exit(0);
		default:
			exit(1);
		}

	if (argc > optind)
		die("too many arguments");

	/* Verify that we're not given two local targets. */
	if (!src.url && !dst.url)
		/* None of -sSdD is specified. */
		usage();
	if (!src.is_local && !src.url)
		/* Input is stdin. */
		src.is_local = 1;
	if (!dst.is_local && !dst.url)
		/* Output is stdout. */
		dst.is_local = 1;
	if (src.is_local && dst.is_local)
		die("at least one iSCSI target must be specified");

	/* Verify that both $src's and $dst's initiators are usable. */
	if (!src.endp.initiator || !src.endp.initiator[0])
		src.endp.initiator = "jaccom";
	if (!dst.endp.initiator)
		dst.endp.initiator = src.endp.initiator;
	else if (!dst.endp.initiator[0])
		dst.endp.initiator = "jaccom";

	/* Both local_to_remote() and remote_to_local() interpret
	 * NULL file names as stdin/stdout. */
	assert(src.is_local || src.url);
	assert(dst.is_local || dst.url);

	/* Make sure we have sane settings.  It's important to leave
	 * local targets' maxreqs zero, because restart_requests()
	 * depends on it. */
	if (!src.is_local && !src.endp.maxreqs)
		src.endp.maxreqs = DFLT_INITIAL_MAX_ISCSI_REQS;
	if (!dst.is_local && !dst.endp.maxreqs)
		dst.endp.maxreqs = DFLT_INITIAL_MAX_ISCSI_REQS;
	if (!Opt_min_output_batch)
		Opt_min_output_batch = 1;
	if (Opt_max_output_queue < Opt_min_output_batch)
		Opt_max_output_queue = Opt_min_output_batch;

	/* Init */
	signal(SIGPIPE, SIG_IGN);
	if (src.is_local)
		/* LOCAL_TO_REMOTE() */
		src.endp.fname = src.fname;
	else if (!init_endpoint(&src.endp, "source", src.url))
		die(NULL);
	if (dst.is_local)
	{	/* REMOTE_TO_LOCAL() */
		if (!dst.fname || !strcmp(dst.fname, "-"))
			/* Output is stdout, don't clobber it with info
			 * messages.  $dst.endp.fname can be left NULL. */
			Info = stderr;
		else	/* Output is NOT stdout. */
			dst.endp.fname = dst.fname;

		/* process_output_queue() needs the block size of the source
		 * in order to calculate the output offset. */
		dst.endp.blocksize = src.endp.blocksize;

		/* Allocate $output->iov and $output->tasks,
		 * which are only needed in this mode. */
		output.max = Opt_max_output_queue;
		output.iov = xmalloc(sizeof(*output.iov) * output.max);
		output.tasks = xmalloc(sizeof(*output.tasks) * output.max);
	} else if (!init_endpoint(&dst.endp, "destination", dst.url))
		die(NULL);
	if (!src.is_local && !dst.is_local)
	{	/* REMOTE_TO_REMOTE().   Unfortunately we can't accept
		 * arbitrary blocksizes, because we can't split/merge
		 * chunks. */
		if (dst.endp.blocksize > src.endp.blocksize)
			die("source target's blocksize must be at least "
				"as large as the destination's");
		else if (src.endp.blocksize % dst.endp.blocksize)
			die("source target's blocksize must be a multiply "
				"of the destination's");
	}

	if (!init_input(&input, &output, &src.endp, &dst.endp))
		die("malloc: %m");
	if (!LOCAL_TO_REMOTE(&input))
		/* Read all blocks of the disk. */
		input.until = src.endp.nblocks;

	/* Run */
	if (nop)
		isok = 1;
	else if (LOCAL_TO_REMOTE(&input))
		isok = local_to_remote(&input);
	else if (REMOTE_TO_LOCAL(&input))
		isok = remote_to_local(&input, output_flags);
	else
		isok = remote_to_remote(&input);

	/* Done */
	if (isok)
	{	/* If we're not $isok, the libiscsi context may be
		 * in inconsistent state, better not to risk using
		 * it anymore. */
   		   if (src.endp.iscsi)
			iscsi_logout_sync(src.endp.iscsi);
		if (dst.endp.iscsi)
			iscsi_logout_sync(dst.endp.iscsi);
	}

	/* Free resources */
	done_input(&input);
	destroy_endpoint(&src.endp);
	destroy_endpoint(&dst.endp);

	exit(!isok);
}
#endif /* SEXYCAT */

/* vim: set foldmarker={{{,}}} foldmethod=marker: */
/* End of sexycat.c */
