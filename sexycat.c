/*
 * sexycat.c -- iSCSI disk-dumper
 */

/* Configuration */
#define _GNU_SOURCE

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <iscsi.h>
#include <scsi-lowlevel.h>

/* Standard definitions */
/* Defaults */
#define DFLT_INITIAL_MAX_ISCSI_REQS	32
#define DFLT_INITIAL_MAX_OUTPUT_QUEUE	(DFLT_INITIAL_MAX_ISCSI_REQS * 2)
#define DFLT_MIN_OUTPUT_BATCH		(DFLT_INITIAL_MAX_OUTPUT_QUEUE / 2)

#define DFLT_ISCSI_MAXREQS_DEGRADATION	50
#define DFLT_ISCSI_REQUEST_RETRY_PAUSE	(3 * 1000)

/* Macros */
/* Return whether we're copying the iSCSI source/destination or not. */
#define LOCAL_TO_REMOTE()		(!Src.iscsi)
#define REMOTE_TO_LOCAL()		(!Dst.iscsi)

#define MEMBS_OF(ary)			(sizeof(ary) / sizeof((ary)[0]))
#define LBA_OF(task)			((task)->params.read10.lba)
#define LBA_OF_CHUNK(chunk)		LBA_OF((chunk)->read_task)

/* Type definitions */
/* Represents an iSCSI source or destination target. */
struct endpoint_st
{
	char const *which;
	union
	{
		char const *fname;
		struct iscsi_url *url;
	};
	struct iscsi_context *iscsi;

	unsigned blocksize, nblocks;
	unsigned nreqs, maxreqs;
};

/* Represents a unit of data being read or written. */
struct chunk_st
{
	struct chunk_st *next;

	unsigned srcblock;
	unsigned time_to_retry;

	union
	{
		struct scsi_task *read_task;
		unsigned char buf[0];
	};
};

/* Function prototypes {{{ */
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
static void *__attribute__((malloc)) xcalloc(size_t size);
static void xrealloc(void *ptrp, size_t size);
static void xpoll(struct pollfd *pfd, unsigned npolls);
static int xfpoll(struct pollfd *pfd, unsigned npolls);
static int xread(int fd, unsigned char *buf, size_t sbuf, size_t *nreadp);
static int xpwritev(int fd, struct iovec const *iov, unsigned niov,
	off_t offset, int seek);

static int is_connection_error(
	struct iscsi_context *iscsi, char const *which,
	unsigned revents);
static int is_iscsi_error(
	struct iscsi_context *iscsi, struct scsi_task *task,
	char const *op, int status);
static void run_iscsi_event_loop(struct iscsi_context *iscsi,
	unsigned events);

static void add_output_chunk(struct chunk_st *chunk);
static void add_to_output_iov(int fd, struct scsi_task *task, unsigned niov);
static int process_output_queue(int fd, int seekable, int more_to_come);

static void chunk_written(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static void chunk_read(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static void restart_requests(void);
static void start_iscsi_read_requests(void);

static void free_chunks(struct chunk_st *chunk);
static void free_surplus_chunks(void);
static void reduce_maxreqs(struct endpoint_st *endp);
static void create_chunks(void);

static void endpoint_connected(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static int connect_endpoint(struct iscsi_context *iscsi,
	struct iscsi_url *url);
static int reconnect_endpoint(struct endpoint_st *endp,
	char const *initiator);

static void destroy_endpoint(struct endpoint_st *endp);
static int init_endpoint(struct endpoint_st *endp, char const *which,
	char const *initiator, char const *url, int is_file);

static int local_to_remote(char const *initiator);
static int remote_to_local(char const *initiator, unsigned output_flags);
static int remote_to_remote(char const *initiator);
/* }}} */

/* Private variables */
/* User controls */
static int Opt_verbosity = 1;
static unsigned Opt_min_output_batch = DFLT_MIN_OUTPUT_BATCH;
static unsigned Opt_max_output_queue = DFLT_INITIAL_MAX_OUTPUT_QUEUE;

static unsigned Opt_maxreqs_degradation = DFLT_ISCSI_MAXREQS_DEGRADATION;
static unsigned Opt_request_retry_time  = DFLT_ISCSI_REQUEST_RETRY_PAUSE;

static char const *Basename;

static struct endpoint_st Src, Dst;
unsigned Src_block_top, Dst_block_top;

unsigned NUnused;
static struct chunk_st *Unused;
static struct chunk_st *Failed, *Last_failed;

static unsigned Output_enqueued;
static struct iovec *Output_iov;
static struct scsi_task **Output_tasks;

/* Program code */
void warnv(char const *fmt, va_list *args)
{
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
	fprintf(stderr, "%s: %s: %m\n", Basename, op);
}

void warn_iscsi(char const *op, struct iscsi_context *iscsi)
{
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

void *xcalloc(size_t size)
{
	void *ptr;

	if ((ptr = xmalloc(size)) != NULL)
		memset(ptr, 0, size);

	return ptr;
}

void xrealloc(void *ptrp, size_t size)
{
	void *ptr;

	ptr = *(void **)ptrp;
	if (!(ptr = realloc(ptr, size)))
		die("remalloc(%zu): %m\n", size);
	*(void **)ptrp = ptr;
}

void xpoll(struct pollfd *pfd, unsigned npolls)
{
	int ret;

	for (;;)
	{
		if ((ret = poll(pfd, npolls, -1)) > 0)
			return;
		assert(ret < 0);
		if (errno != EINTR)
			die("poll: %m");
	}
}

int xfpoll(struct pollfd *pfd, unsigned npolls)
{
	int timeout, ret;
	struct timespec then, now;

	if (Failed)
	{
		timeout = Failed->time_to_retry;
		clock_gettime(CLOCK_MONOTONIC, &then);
	} else
		timeout = -1;

	for (;;)
	{
		if ((ret = poll(pfd, npolls, timeout)) >= 0)
			break;
		if (errno != EINTR)
			die("poll: %m");
	}

	if (Failed)
	{
		unsigned diff;
		struct chunk_st *chunk;
		const unsigned ms_per_sec = 1000;
		const unsigned long ns_per_ms = 1000000;

		/* $diff := $now - $then */
		clock_gettime(CLOCK_MONOTONIC, &now);
		diff = (now.tv_sec - then.tv_sec) * ms_per_sec;
		if (now.tv_nsec < then.tv_nsec)
			diff += ms_per_sec;
		diff += (now.tv_nsec - then.tv_nsec) / ns_per_ms;

		/* Discount $diff milliseconds from all $Failed chunks. */
		for (chunk = Failed; chunk; chunk = chunk->next)
			if (chunk->time_to_retry > diff)
				chunk->time_to_retry -= diff;
			else
				chunk->time_to_retry = 0;
	}

	return ret != 0;
}

int xread(int fd, unsigned char *buf, size_t sbuf, size_t *nreadp)
{
	*nreadp = 0;
	while (*nreadp < sbuf)
	{
		ssize_t n;

		n = read(fd, &buf[*nreadp], sbuf - *nreadp);
		if (n > 0)
			*nreadp += n;
		else if (n == 0)
			return 1;
		else if (errno != EAGAIN && errno != EINTR
				&& errno != EWOULDBLOCK)
			return 0;
	}

	return 1;
}

int xpwritev(int fd, struct iovec const *iov, unsigned niov, off_t offset,
	int seek)
{
	int ret;

	assert(fd >= 0);
	ret = seek
		? niov > 1
			? pwritev(fd, iov, niov, offset)
			: pwrite(fd, iov[0].iov_base, iov[0].iov_len, offset)
		: niov > 1
			? writev(fd, iov, niov)
			: write(fd, iov[0].iov_base, iov[0].iov_len);
	return ret > 0;
}

int is_connection_error(struct iscsi_context *iscsi, char const *which,
	unsigned revents)
{
	int error;
	socklen_t serror;

	if (!(revents & (POLLERR|POLLHUP|POLLRDHUP)))
		return 0;

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

void run_iscsi_event_loop(struct iscsi_context *iscsi, unsigned events)
{
	if (iscsi_service(iscsi, events) != 0)
	{
		warn_iscsi(NULL, iscsi);
		die(NULL);
	}

}

void add_output_chunk(struct chunk_st *chunk)
{
	unsigned lba, i;

	/* Make room for $chunk in $Output_tasks if it's full. */
	if (Output_enqueued >= Opt_max_output_queue)
	{
		unsigned n;

		/* Allocate +25% */
		n = Opt_max_output_queue + Opt_max_output_queue/4;
		xrealloc(&Output_tasks, sizeof(*Output_tasks) * n);
		xrealloc(&Output_iov, sizeof(*Output_iov) * n);
		Opt_max_output_queue = n;
	}

	/* Find a place for $chunk in $Output_tasks in which buffers
	 * are ordered by LBA. */
	assert(Output_enqueued < Opt_max_output_queue);
	lba = LBA_OF_CHUNK(chunk);
	for (i = Output_enqueued; i > 0; i--)
		if (LBA_OF(Output_tasks[i-1]) < lba)
			break;

	/* Insert $chunk->read_task into $Output_tasks. */
	memmove(&Output_tasks[i+1], &Output_tasks[i],
		sizeof(*Output_tasks) * (Output_enqueued - i));
	Output_tasks[i] = chunk->read_task;
	chunk->read_task = NULL;
	Output_enqueued++;

	/* Return $chunk to $Unused. */
	chunk->next = Unused;
	Unused = chunk;
	NUnused++;
}

void add_to_output_iov(int fd, struct scsi_task *task, unsigned niov)
{
	if (fd < 0)
		return;
	assert(niov < Opt_max_output_queue);
	Output_iov[niov].iov_base = task->datain.data;
	Output_iov[niov].iov_len  = task->datain.size;
}

int process_output_queue(int fd, int seekable, int more_to_come)
{
	int need_to_seek;
	unsigned niov, ntasks;
	unsigned first, block;
	struct scsi_task **tasks, **from_task, **t;

	/*
	 * $niov	:= the number of buffers in the current batch
	 * $first	:= the output offset of the batch
	 * $block	:= the next block we expect in the batch
	 * $tasks	:= where to take from the next buffer of the batch
	 * $ntasks	:= how many buffers till the end of $Output_tasks
	 * $from_task	:= the first buffer in the batch
	 * $need_to_seek := whether the current position of $fd is $first
	 */
	niov = 0;
	need_to_seek = 0;
	ntasks = Output_enqueued;
	first = block = Dst_block_top;
	tasks = from_task = Output_tasks;

	/* Send all of $Output_enqueued batches. */
	assert(Opt_max_output_queue > 0);
	for (;;)
	{
		if (niov >= Opt_max_output_queue)
		{	/* $Output_iov has reached its maximal capacity,
			 * we need to flush it.  Fall through. */
		} else if (!ntasks)
		{	/* We've run out of output.  Flush or break? */
			if (niov < Opt_min_output_batch && more_to_come)
				/* Too little output to flush and there's
				 * $more_to_come. */
				break;
			/* Fall through. */
		} else if (LBA_OF(*tasks) == block)
		{	/* Found the next buffer in $Output_tasks. */
			block++;
			ntasks--;
			add_to_output_iov(fd, *tasks++, niov++);
			continue;
		} else if (niov >= Opt_min_output_batch)
		{	/* The current batch has finished and there's
			 * enough output to flush.  Fall through. */
		} else if (seekable)
		{	/* The current batch is too small to output,
			 * but we can see the next one, because the
			 * output is $seekable. */
			/* The next batch starts from *tasks. */
			first = LBA_OF(*tasks);
			block = first + 1;
			from_task = tasks;
			add_to_output_iov(fd, *tasks++, 0);
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
			 * output is not $seekable.  Wait for more output. */
			break;

		/* Flush $Output_iov. */
		if (!niov)
			/* ...but it's empty. */
			return 0;
		if (fd < 0)
			/* We would have flushed something. */
			return 1;

		/* Write the buffers to $fd. */
		if (!xpwritev(fd, Output_iov, niov, Dst.blocksize * first,
				need_to_seek))
			die("%s: %m", Dst.fname ? Dst.fname : "(stdout)");

		/* Delete output buffers. */
		for (t = from_task; t < tasks; t++)
			scsi_free_scsi_task(*t);
		memmove(from_task, tasks,
			sizeof(*tasks) * (tasks - from_task));
		Output_enqueued = ntasks;

		/* If we've flushed the first possible batch, update
		 * $Dst_block_top, which then will point to the start
		 * of the next possible batch. */
		if (Dst_block_top == first)
			Dst_block_top = block;
		first = block;

		niov = 0;
		need_to_seek = 0;
	} /* until all batches are output or skipped */

	return 0;
}

void chunk_failed(struct chunk_st *chunk)
{
	/* Append $chunk to $Failed. */
	assert(!chunk->next);

	if (!Failed)
	{
		assert(!Last_failed);
		Failed = chunk;
	} else
	{
		assert(Last_failed);
		assert(!Last_failed->next);
		Last_failed->next = chunk;
	}

	Last_failed = chunk;
	chunk->time_to_retry = Opt_request_retry_time;
}

void chunk_written(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data)
{
	struct scsi_task *task = command_data;
	struct chunk_st *chunk = private_data;

	assert(!REMOTE_TO_LOCAL());
	assert(LOCAL_TO_REMOTE() || chunk->read_task);
	assert(Dst.nreqs > 0);
	Dst.nreqs--;

	if (is_iscsi_error(iscsi, task, "write10", status))
	{
		scsi_free_scsi_task(task);
		chunk_failed(chunk);
		return;
	}

	if (Opt_verbosity > 1)
		printf("source block %u copied\n", chunk->srcblock);
	scsi_free_scsi_task(task);

	chunk->srcblock = 0;
	assert(!chunk->time_to_retry);
	if (!LOCAL_TO_REMOTE())
	{	/* remote_to_remote() */
		scsi_free_scsi_task(chunk->read_task);
		chunk->read_task = NULL;
	}

	/* Return $chunk to $Unused. */
	chunk->next = Unused;
	Unused = chunk;
	NUnused++;
}

void chunk_read(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data)
{
	struct scsi_task *task = command_data;
	struct chunk_st *chunk = private_data;

	assert(!LOCAL_TO_REMOTE());
	assert(!chunk->read_task);
	assert(Src.nreqs > 0);
	Src.nreqs--;

	if (is_iscsi_error(iscsi, task, "read10", status))
	{
		scsi_free_scsi_task(task);
		chunk_failed(chunk);
		return;
	}

	if (Opt_verbosity > 2)
		printf("source block %u read\n", chunk->srcblock);

	chunk->read_task = task;
	assert(!chunk->time_to_retry);
	if (!REMOTE_TO_LOCAL())
	{	/* remote_to_remote() */
		if (!iscsi_write10_task(
			Dst.iscsi, Dst.url->lun,
			task->datain.data, task->datain.size,
			chunk->srcblock, 0, 0, Dst.blocksize,
			chunk_written, chunk))
		{
			warn_iscsi("write10", Dst.iscsi);
			die(NULL);
		}
		Dst.nreqs++;
	} else	/* remote_to_local() */
		add_output_chunk(chunk);
}

void restart_requests(void)
{
	struct chunk_st *prev, *chunk, *next;

	/* Do we have anything to do? */
	if (!(chunk = Failed))
		return;

	/* Can we send any requests at all? */
	if (!(Src.nreqs < Src.maxreqs || Dst.nreqs < Dst.maxreqs))
		return;

	/* We need to know the current time to tell whether a failed request
	 * can be retried. */
	prev = NULL;
	do
	{	/* As long as we have $Failed requests which have reached
		 * $time_to_retry.  Since the list is ordered, the first time
		 * we meet a $chunk still not ready to retry, we can stop. */
		if (chunk->time_to_retry)
			break;

		/* Reissue the failed request if possible. */
		next = chunk->next;
		if (!LOCAL_TO_REMOTE() && !chunk->read_task)
		{	/* Re-read */
			if (!(Src.nreqs < Src.maxreqs))
			{	/* Max number of reqs reached. */
				prev = chunk;
				continue;
			}

			if (Opt_verbosity > 3)
				printf("re-reading source block %u\n",
					chunk->srcblock);
			if (!iscsi_read10_task(
				Src.iscsi, Src.url->lun,
				chunk->srcblock, Src.blocksize,
				Src.blocksize,
				chunk_read, chunk))
			{	/* It must be some fatal error, eg. OOM. */
				warn_iscsi("read10", Src.iscsi);
				die(NULL);
			}
			Src.nreqs++;
		} else	/* LOCAL_TO_REMOTE() || chunk->read_task != NULL */
		{	/* Rewrite */
			size_t sbuf;
			unsigned char *buf;

			if (!(Dst.nreqs < Dst.maxreqs))
			{	/* Max number of reqs reached. */
				prev = chunk;
				continue;
			}

			if (Opt_verbosity > 3)
				printf("rewriting source block %u\n",
					chunk->srcblock);

			if (LOCAL_TO_REMOTE())
			{	/* In this mode the buffer comes right after
				 * the struct chunk_st. */
				buf  = chunk->buf;
				sbuf = Dst.blocksize;
			} else
			{	/* remote_to_remote() */
				buf  = chunk->read_task->datain.data;
				sbuf = chunk->read_task->datain.size;
			}

			if (!iscsi_write10_task(
				Dst.iscsi, Dst.url->lun,
				buf, sbuf, chunk->srcblock, 0, 0,
				Dst.blocksize, chunk_written, chunk))
			{	/* Incorrectible error. */
				warn_iscsi("write10", Dst.iscsi);
				die(NULL);
			}
			Dst.nreqs++;
		}

		/* Unlink $chunk from $Failed and update $prev,
		 * $Failed and $Last_failed. */
		chunk->next = NULL;
		if (chunk == Failed)
		{
			assert(!prev);
			Failed = next;
		} else
		{
			assert(prev != NULL);
			prev->next = next;
		}
		if (chunk == Last_failed)
			Last_failed = prev;
	} while ((chunk = next) != NULL);
}

void start_iscsi_read_requests(void)
{
	/* Issue new read requests as long as we can. */
	assert(!LOCAL_TO_REMOTE());
	while (Unused
		&& Src.nreqs < Src.maxreqs
		&& Src_block_top < Src.nblocks)
	{
		struct chunk_st *chunk;

		chunk = Unused;
		assert(!chunk->read_task);
		assert(!chunk->time_to_retry);

		if (Opt_verbosity > 3)
			printf("reading source block %u\n",
				Src_block_top);

		if (!iscsi_read10_task(
			Src.iscsi, Src.url->lun,
			Src_block_top, Src.blocksize,
			Src.blocksize,
			chunk_read, chunk))
		{
			warn_iscsi("read10", Dst.iscsi);
			die(NULL);
		}
		chunk->srcblock = Src_block_top++;

		/* Detach $chunk from $Unused. */
		Src.nreqs++;
		NUnused--;
		Unused = chunk->next;
		chunk->next = NULL;
	} /* read until there are no $Unused chunks left */
}

void free_chunks(struct chunk_st *chunk)
{
	while (chunk)
	{
		struct chunk_st *next;

		next = chunk->next;
		if (!LOCAL_TO_REMOTE() && chunk->read_task)
			scsi_free_scsi_task(chunk->read_task);
		chunk = next;
	}
}

void free_surplus_chunks(void)
{
	unsigned maxreqs;
	struct chunk_st *chunk;

	/* Free $Unused until $NUnused drops to $maxreqs. */
	maxreqs = Src.maxreqs + Dst.maxreqs;
	assert(maxreqs >= 1);
	while (NUnused > maxreqs)
	{
		chunk = Unused;
		assert(chunk != NULL);
		assert(LOCAL_TO_REMOTE() || !chunk->read_task);
		Unused = chunk->next;
		free(chunk);
		NUnused--;
	}
}

void reduce_maxreqs(struct endpoint_st *endp)
{
	unsigned maxreqs;

	/* Decrease the maximum number of requests? */
	if (!Opt_maxreqs_degradation || Opt_maxreqs_degradation == 100)
		return;
	assert(Opt_maxreqs_degradation < 100);

	/* Calculate the new maxreqs of $endp. */
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

	free_surplus_chunks();
	printf("%s target: number of maximal outstanding requests "
	       "reduced to %u\n", endp->which, endp->maxreqs);
}

void create_chunks(void)
{
	unsigned nchunks;
	size_t inline_buf_size;

	/*
	 * If we're copying a local file to a remote target the input buffer
	 * is allocated together with the $chunk, the size of which is
	 * $Dst.blocksize.  In this case we need to discount the space
	 * allocated for the $read_task pointer, because its size is included
	 * in the size of the structure.
	 */
	assert(REMOTE_TO_LOCAL()
		|| Dst.blocksize > sizeof(struct scsi_task *));
	inline_buf_size = LOCAL_TO_REMOTE()
		? Dst.blocksize - sizeof(struct scsi_task *): 0;
	for (nchunks = Src.maxreqs + Dst.maxreqs; nchunks > 0; nchunks--)
	{
		struct chunk_st *chunk;

		chunk = xcalloc(sizeof(*chunk) + inline_buf_size);
		chunk->next = Unused;
		Unused = chunk;
		NUnused++;
	} /* until $Src.maxreqs + $Dst.maxreqs $Unused chunks are created */
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
		xpoll(&pfd, MEMBS_OF(&pfd));

		run_iscsi_event_loop(iscsi, pfd.revents);
		if (!connected)
		{
			warn("connect: %s: %s: %s\n",
				url->portal, url->target,
				iscsi_get_error(iscsi));
			return 0;
		}
	} while (connected < 0);

	return 1;
}

int reconnect_endpoint(struct endpoint_st *endp, char const *initiator)
{
	iscsi_destroy_context(endp->iscsi);
	if (!(endp->iscsi = iscsi_create_context(initiator)))
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
	} else
		endp->url = NULL;

	if (endp->url)
	{
		iscsi_destroy_url(endp->url);
		endp->url = NULL;
	}

	endp->which = NULL;
}

int init_endpoint(struct endpoint_st *endp, char const *which,
	char const *initiator, char const *url, int is_file)
{
	struct scsi_task *task;
	struct scsi_readcapacity10 *cap;

	if (is_file)
	{	/* Endpoint is local, $blocksize is irrelevant. */
		endp->which = which;
		endp->fname = url;
		if (Opt_verbosity > 0)
			printf("%s is local\n", which);
		return 1;
	} else if (!(endp->iscsi = iscsi_create_context(initiator)))
	{
		warn_errno("iscsi_create_context()");
		return 0;
	} else if (!(endp->url = iscsi_parse_full_url(endp->iscsi, url)))
	{
		warn_iscsi(NULL, endp->iscsi);
		destroy_endpoint(endp);
		return 0;
	} else if (!connect_endpoint(endp->iscsi, endp->url))
	{
		destroy_endpoint(endp);
		return 0;
	} else
		endp->which = which;

	/* Get the endpoint's nblocks and blocksize. */
	if (!(task = iscsi_readcapacity10_sync(endp->iscsi, endp->url->lun,
		0, 0)))
	{
		warn_iscsi("readcapacity10", endp->iscsi);
		destroy_endpoint(endp);
		return 0;
	} else if (!(cap = scsi_datain_unmarshall(task)))
	{
		warn_errno("readcapacity10");
		destroy_endpoint(endp);
		return 0;
	} else
	{
		endp->blocksize = cap->block_size;
		endp->nblocks = cap->lba + 1;
		scsi_free_scsi_task(task);
	}
	if (!endp->blocksize)
	{
		warn("%s target reported blocksize=0, ignored\n", which);
		endp->blocksize = 512;
	}

#if 0
	/* IIRC this could be used to determine the optimal nblocks */
	struct scsi_inquiry_block_limits *inq;
	task = iscsi_inquiry_sync(endp->iscsi, endp->url->lun,
		1, SCSI_INQUIRY_PAGECODE_BLOCK_LIMITS, sizeof(*inq));
	inq = scsi_datain_unmarshall(task);
#endif

	if (Opt_verbosity > 0)
		printf("%s target: blocksize=%u, nblocks=%u\n",
			which, endp->blocksize, endp->nblocks);

	return 1;
}

int local_to_remote(char const *initiator)
{
	int eof;
	struct pollfd pfd[2];

	/* Open the input file. */
	if (!Src.fname || !strcmp(Src.fname, "-"))
	{	/* Input is stdin. */
		Src.fname = NULL;
		pfd[0].fd = STDIN_FILENO;
	} else if ((pfd[0].fd = open(Src.fname, O_RDONLY)) < 0)
	{
		warn_errno(Src.fname);
		return 0;
	}

	eof = 0;
	pfd[1].fd = iscsi_get_fd(Dst.iscsi);
	for (;;)
	{
		restart_requests();
		if (eof && !Dst.nreqs && !Failed)
			break;

		pfd[0].events = !eof && Unused ? POLLIN : 0;
		pfd[1].events = iscsi_which_events(Dst.iscsi);
		if (!xfpoll(pfd, MEMBS_OF(pfd)))
			continue;

		if (pfd[0].revents)
		{
			size_t n;
			struct chunk_st *chunk;

			assert(!eof);
			chunk = Unused;
			assert(chunk != NULL);
			if (!xread(pfd[0].fd, chunk->buf, Dst.blocksize, &n))
			{
				warn_errno(Src.fname ? Src.fname : "(stdin)");
				return 0;
			}

			if (n < Dst.blocksize)
				eof = 1;
			if (n > 0)
			{
				/* Remove $chunk from $Unused. */
				NUnused--;
				Unused = chunk->next;
				chunk->next = NULL;
				chunk->srcblock = Src_block_top++;

				assert(n <= Dst.blocksize);
				if (n < Dst.blocksize)
				{
					warn("source block %u "
						"padded with zeroes",
						chunk->srcblock);
					memset(&chunk->buf[n], 0,
						Dst.blocksize - n);
				}

				if (!iscsi_write10_task(
					Dst.iscsi, Dst.url->lun,
					chunk->buf, Dst.blocksize,
					chunk->srcblock, 0, 0,
					Dst.blocksize,
					chunk_written, chunk))
				{
					warn_iscsi("write10", Dst.iscsi);
					die(NULL);
				}
				Dst.nreqs++;
			}
		}

		if (!is_connection_error(Dst.iscsi, Dst.which,
			pfd[1].revents))
		{
			run_iscsi_event_loop(Dst.iscsi, pfd[1].revents);
			free_surplus_chunks();
		} else if (reconnect_endpoint(&Dst, initiator))
			reduce_maxreqs(&Dst);
		else
			return 0;
	} /* until $eof is reached and everything is written out */

	/* Close the input file if we opened it. */
	if (!Src.fname)
		close(pfd[0].fd);

	return 1;
}

int remote_to_local(char const *initiator, unsigned output_flags)
{
	int seekable;
	struct pollfd pfd[2];

	/* Open the output file. */
	output_flags |= O_CREAT | O_WRONLY;
	if (!Dst.fname || !strcmp(Dst.fname, "-"))
	{	/* Output is stdout. */
		Dst.fname = NULL;
		pfd[1].fd = STDOUT_FILENO;
	} else if ((pfd[1].fd = open(Dst.fname, output_flags, 0666)) < 0)
	{
		warn_errno(Dst.fname);
		return 0;
	}

	/* If the output is $seekable we'll possibly use pwrite().
	 * In this case we need to allocate space for it, otherwise
	 * it will return ESPIPE. */
	seekable = lseek(pfd[1].fd, 0, SEEK_CUR) != (off_t)-1;
	if (seekable && ftruncate(pfd[1].fd, Src.blocksize * Src.nblocks) < 0)
	{
		warn_errno(Dst.fname);
		return 0;
	}

	pfd[0].fd = iscsi_get_fd(Src.iscsi);
	for (;;)
	{
		int eof;

		restart_requests();
		start_iscsi_read_requests();
		eof = !Src.nreqs && !Failed;
		if (eof && !Output_enqueued)
			break;

		pfd[0].events = iscsi_which_events(Src.iscsi);
		pfd[1].events = process_output_queue(-1, seekable, !eof)
			? POLLOUT : 0;
		if (!xfpoll(pfd, MEMBS_OF(pfd)))
			continue;

		if (!is_connection_error(Src.iscsi, Src.which,
				pfd[0].revents))
			run_iscsi_event_loop(Src.iscsi, pfd[0].revents);
		else if (reconnect_endpoint(&Src, initiator))
			reduce_maxreqs(&Src);
		else
			return 0;

		if (pfd[1].revents)
		{
			process_output_queue(pfd[1].fd, seekable, !eof);
			free_surplus_chunks();
		}
	}

	/* Close the input file if we opened it. */
	if (!Dst.fname)
		close(pfd[0].fd);

	return 1;
}

int remote_to_remote(char const *initiator)
{
	struct pollfd pfd[2];

	pfd[0].fd = iscsi_get_fd(Src.iscsi);
	pfd[1].fd = iscsi_get_fd(Dst.iscsi);
	for (;;)
	{
		restart_requests();
		start_iscsi_read_requests();

		if (!Src.nreqs && !Dst.nreqs && !Failed)
			break;

		pfd[0].events = iscsi_which_events(Src.iscsi);
		pfd[1].events = iscsi_which_events(Dst.iscsi);
		if (!xfpoll(pfd, MEMBS_OF(pfd)))
			continue;

		if (!is_connection_error(Src.iscsi, Src.which,
				pfd[0].revents))
			run_iscsi_event_loop(Src.iscsi, pfd[0].revents);
		else if (reconnect_endpoint(&Src, initiator))
			reduce_maxreqs(&Src);
		else
			return 0;

		if (!is_connection_error(Dst.iscsi, Dst.which,
			pfd[1].revents))
		{
			run_iscsi_event_loop(Dst.iscsi, pfd[1].revents);
			free_surplus_chunks();
		} else if (reconnect_endpoint(&Dst, initiator))
			reduce_maxreqs(&Dst);
		else
			return 0;
	}

	return 1;
}

/* The main function */
int main(int argc, char *argv[])
{
	struct
	{
		int is_file;
		union
		{
			char const *url;
			char const *fname;
		};
	} src, dst;
	int isok, optchar;
	unsigned output_flags;
	char const *initiator;

	/* Initialize diagnostics. */
	if ((Basename = strrchr(argv[0], '/')) != NULL)
		Basename++;
	else
		Basename = argv[0];
	setvbuf(stderr, NULL, _IOLBF, 0);

	/* Parse the command line */
	output_flags = O_EXCL;
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));

	/* These defaults are used in --debug mode. */
	initiator = "jaccom";
	src.url = "iscsi://127.0.0.1/iqn.2014-07.net.nsn-net.timmy:omu/0";
	dst.url = "iscsi://127.0.0.1/iqn.2014-07.net.nsn-net.timmy:omu/1";
	if (argv[1] && !strcmp(argv[1], "--debug"))
	{
		argc--;
		argv++;
	} else
		initiator = src.url = dst.url = NULL;

	while ((optchar = getopt(argc, argv, "vqi:s:S:m:d:D:M:Or:R:")) != EOF)
		switch (optchar)
		{
		case 'v':
			Opt_verbosity++;
			break;
		case 'q':
			Opt_verbosity--;
			break;
		case 'i':
			initiator = optarg;
			break;
		case 's':
			src.url = optarg;
			break;
		case 'S':
			src.is_file = 1;
			src.fname = optarg;
			break;
		case 'm':
			Src.maxreqs = atoi(optarg);
			break;
		case 'd':
			dst.url = optarg;
			break;
		case 'D':
			dst.is_file = 1;
			dst.fname = optarg;
			break;
		case 'M':
			Dst.maxreqs = atoi(optarg);
			break;
		case 'O':
			output_flags &= ~O_EXCL;
			output_flags |= O_TRUNC;
			break;
		case 'r':
			Opt_request_retry_time = atoi(optarg);
			break;
		case 'R':
			Opt_maxreqs_degradation = atoi(optarg);
			if (Opt_maxreqs_degradation > 100)
				die("maximum iSCSI requests "
					"degradation must be under 100%%");
			break;
		default:
			return 1;
		}

	/* Verify that we're not given two local targets. */
	if ((!src.url && !dst.url)
		|| (src.is_file && dst.is_file)
		|| (src.is_file && !dst.url)
		|| (dst.is_file && !src.url))
	{
		die("at least one iSCSI target must be specified");
	} else if (!src.url)
	{
		src.is_file = 1;
		Src.maxreqs = 0;
	} else if (!dst.url)
	{
		dst.is_file = 1;
		Dst.maxreqs = 0;
	}

	/* Make sure we have sane settings. */
	if (!Src.maxreqs)
		Src.maxreqs = DFLT_INITIAL_MAX_ISCSI_REQS;
	if (!Dst.maxreqs)
		Dst.maxreqs = DFLT_INITIAL_MAX_ISCSI_REQS;
	if (!Opt_min_output_batch)
		Opt_min_output_batch = 1;
	if (Opt_max_output_queue < Opt_min_output_batch)
		Opt_max_output_queue = Opt_min_output_batch;

	/* Init */
	signal(SIGPIPE, SIG_IGN);
	if (!init_endpoint(&Src, "source", initiator,
			src.url, src.is_file))
		die(NULL);
	if (!init_endpoint(&Dst, "destination", initiator,
			dst.url, dst.is_file))
		die(NULL);
	create_chunks();

	/* Run */
	if (LOCAL_TO_REMOTE())
		isok = local_to_remote(initiator);
	else if (REMOTE_TO_LOCAL())
	{	/* Allocate $Output_iov and $Output_tasks,
		 * which are only needed in this mode. */
		Output_iov = xmalloc(
			sizeof(*Output_iov) * Opt_max_output_queue);
		Output_tasks = xmalloc(
			sizeof(*Output_tasks) * Opt_max_output_queue);
		isok = remote_to_local(initiator, output_flags);
	} else
		isok = remote_to_remote(initiator);

	/* Done */
	if (isok)
	{	/* If we're not $isok, the libiscsi context may be
		 * in inconsistent state. */
   		   if (Src.iscsi)
			iscsi_logout_sync(Src.iscsi);
		if (Dst.iscsi)
			iscsi_logout_sync(Dst.iscsi);
	}

	/* Free resources */
	free_chunks(Unused);
	free_chunks(Failed);
	Unused = Failed = NULL;
	destroy_endpoint(&Src);
	destroy_endpoint(&Dst);

	return !isok;
}

/* vim: set foldmarker={{{,}}} foldmethod=marker: */
/* End of sexycat.c */
