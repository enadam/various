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
#define LOCAL_TO_REMOTE(input)		(!(input)->src->iscsi)
#define REMOTE_TO_LOCAL(input)		(!(input)->dst->iscsi)

#define MEMBS_OF(ary)			(sizeof(ary) / sizeof((ary)[0]))
#define LBA_OF(task)			((task)->params.read10.lba)
#define LBA_OF_CHUNK(chunk)		LBA_OF((chunk)->read_task)

/* Type definitions */
typedef unsigned long scsi_block_addr_t;
typedef unsigned long scsi_block_count_t;

/* Represents an iSCSI source or destination target. */
struct endpoint_st
{
	union
	{
		char const *fname;
		struct iscsi_url *url;
	};
	struct iscsi_context *iscsi;

	/* Zero in case of local target. */
	unsigned maxreqs;

	/* The source's block size in case of local destination. */
	unsigned blocksize;

	union
	{
		struct
		{	/* Used for remote targets. */
			scsi_block_count_t nblocks;
			unsigned granuality, optimum;
		};

		/* Used for local destination. */
		int seekable;
	};
};

/* Represents a unit of data being read or written in a single request. */
struct input_st;
struct chunk_st
{
	struct chunk_st *next;
	struct input_st *input;

	scsi_block_addr_t srcblock;
	unsigned time_to_retry;

	union
	{
		struct scsi_task *read_task;
		unsigned char buf[0];
	};
};

struct output_st
{
	union
	{
		/* This is only for remote destination. */
		unsigned nreqs;

		struct
		{	/* These are only used for local destination. */
			unsigned max;
			unsigned enqueued;
			struct iovec *iov;
			struct scsi_task **tasks;
			scsi_block_addr_t top_block;
		};
	};
};

struct input_st
{
	unsigned nreqs;
	scsi_block_addr_t top_block;

	unsigned nunused;
	struct chunk_st *unused;
	struct chunk_st *failed, *last_failed;

	struct output_st *output;
	struct endpoint_st *src, *dst;
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
static int xfpoll(struct pollfd *pfd, unsigned npolls, struct input_st *input);
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
static void add_to_output_iov(struct output_st *output,
	struct scsi_task *task, unsigned niov);
static int process_output_queue(int fd,
	struct endpoint_st const *dst, struct output_st *output,
	int more_to_come);

static void chunk_written(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static void chunk_read(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static void restart_requests(struct input_st *input);
static void start_iscsi_read_requests(struct input_st *input);

static void free_chunks(struct chunk_st *chunk);
static void free_surplus_unused_chunks(struct input_st *input);
static void reduce_maxreqs(struct endpoint_st *endp, char const *which);
static void return_chunk(struct chunk_st *chunk);
static void chunk_failed(struct chunk_st *chunk);
static void create_chunks(struct input_st *input, unsigned nchunks);

static void endpoint_connected(struct iscsi_context *iscsi, int status,
	void *command_data, void *private_data);
static int connect_endpoint(struct iscsi_context *iscsi,
	struct iscsi_url *url);
static int reconnect_endpoint(struct endpoint_st *endp,
	char const *initiator);

static void destroy_endpoint(struct endpoint_st *endp);
static int init_endpoint(struct endpoint_st *endp, char const *which,
	char const *initiator, char const *url, int is_file);

static int local_to_remote(char const *initiator, struct input_st *input);
static int remote_to_local(char const *initiator, struct input_st *input,
	unsigned output_flags);
static int remote_to_remote(char const *initiator, struct input_st *input);
/* }}} */

/* Private variables */
/* User controls */
static int Opt_verbosity = 1;
static unsigned Opt_min_output_batch = DFLT_MIN_OUTPUT_BATCH;
static unsigned Opt_max_output_queue = DFLT_INITIAL_MAX_OUTPUT_QUEUE;

static unsigned Opt_maxreqs_degradation = DFLT_ISCSI_MAXREQS_DEGRADATION;
static unsigned Opt_request_retry_time  = DFLT_ISCSI_REQUEST_RETRY_PAUSE;

static char const *Basename;

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

int xfpoll(struct pollfd *pfd, unsigned npolls, struct input_st *input)
{
	int timeout, ret;
	struct timespec then, now;

	if (input->failed)
	{
		timeout = input->failed->time_to_retry;
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

	if (input->failed)
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

		/* Discount $diff milliseconds from all failed chunks. */
		for (chunk = input->failed; chunk; chunk = chunk->next)
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
	struct output_st *output = chunk->input->output;

	/* Make room for $chunk in $output->tasks if it's full. */
	if (output->enqueued >= output->max)
	{
		unsigned n;

		/* Allocate +25% */
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
	 * $first	:= the output offset of the batch
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

	assert(!REMOTE_TO_LOCAL(input));
	assert(LOCAL_TO_REMOTE(input) || chunk->read_task);
	assert(input->output->nreqs > 0);
	input->output->nreqs--;

	if (is_iscsi_error(iscsi, task, "write10", status))
	{
		scsi_free_scsi_task(task);
		chunk_failed(chunk);
		return;
	} else
		scsi_free_scsi_task(task);

	if (Opt_verbosity > 1)
		printf("source block %lu copied\n", chunk->srcblock);

	chunk->srcblock = 0;
	assert(!chunk->time_to_retry);
	if (!LOCAL_TO_REMOTE(input))
	{	/* remote_to_remote() */
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

	assert(!LOCAL_TO_REMOTE(chunk->input));
	assert(!chunk->read_task);
	assert(chunk->input->nreqs > 0);
	chunk->input->nreqs--;

	if (is_iscsi_error(iscsi, task, "read10", status))
	{
		scsi_free_scsi_task(task);
		chunk_failed(chunk);
		return;
	}

	if (Opt_verbosity > 2)
		printf("source block %lu read\n", chunk->srcblock);

	chunk->read_task = task;
	assert(!chunk->time_to_retry);
	if (!REMOTE_TO_LOCAL(chunk->input))
	{	/* remote_to_remote() */
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
	} else	/* remote_to_local() */
		add_output_chunk(chunk);
}

void restart_requests(struct input_st *input)
{
	struct chunk_st *prev, *chunk, *next;
	struct output_st *output = input->output;
	struct endpoint_st *src = input->src;
	struct endpoint_st *dst = input->dst;

	/* Do we have anything to do? */
	if (!(chunk = input->failed))
		return;

	/* Can we send any requests at all? */
	if (!(input->nreqs < src->maxreqs || output->nreqs < dst->maxreqs))
		return;

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

			if (Opt_verbosity > 3)
				printf("re-reading source block %lu\n",
					chunk->srcblock);
			if (!iscsi_read10_task(
				src->iscsi, src->url->lun,
				chunk->srcblock, src->blocksize,
				src->blocksize, chunk_read, chunk))
			{	/* It must be some fatal error, eg. OOM. */
				warn_iscsi("read10", src->iscsi);
				die(NULL);
			} else
				input->nreqs++;
		} else	/* LOCAL_TO_REMOTE() || chunk->read_task != NULL */
		{	/* Rewrite */
			size_t sbuf;
			unsigned char *buf;

			assert(!REMOTE_TO_LOCAL(input));
			if (!(output->nreqs < dst->maxreqs))
			{	/* Max number of reqs reached. */
				prev = chunk;
				continue;
			}

			if (Opt_verbosity > 3)
				printf("rewriting source block %lu\n",
					chunk->srcblock);

			if (LOCAL_TO_REMOTE(input))
			{	/* In this mode the buffer comes right after
				 * the struct chunk_st. */
				buf  = chunk->buf;
				sbuf = dst->blocksize;
			} else
			{	/* remote_to_remote() */
				buf  = chunk->read_task->datain.data;
				sbuf = chunk->read_task->datain.size;
			}

			if (!iscsi_write10_task(
				dst->iscsi, dst->url->lun,
				buf, sbuf, chunk->srcblock, 0, 0,
				dst->blocksize, chunk_written, chunk))
			{	/* Incorrectible error. */
				warn_iscsi("write10", dst->iscsi);
				die(NULL);
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
}

void start_iscsi_read_requests(struct input_st *input)
{
	struct endpoint_st *src = input->src;

	/* Issue new read requests as long as we can. */
	assert(!LOCAL_TO_REMOTE(input));
	while (input->unused
		&& input->nreqs < src->maxreqs
		&& input->top_block < src->nblocks)
	{
		struct chunk_st *chunk;

		chunk = input->unused;
		assert(!chunk->read_task);
		assert(!chunk->time_to_retry);

		if (Opt_verbosity > 3)
			printf("reading source block %lu\n",
				input->top_block);

		if (!iscsi_read10_task(
			src->iscsi, src->url->lun,
			input->top_block, src->blocksize,
			src->blocksize, chunk_read, chunk))
		{
			warn_iscsi("read10", src->iscsi);
			die(NULL);
		}
		chunk->srcblock = input->top_block++;

		/* Detach $chunk from $input->unused. */
		input->nreqs++;
		input->nunused--;
		input->unused = chunk->next;
		chunk->next = NULL;
	} /* read until there are no $input->unused chunks left */
}

void free_chunks(struct chunk_st *chunk)
{
	while (chunk)
	{
		struct chunk_st *next;

		next = chunk->next;
		if (!LOCAL_TO_REMOTE(chunk->input) && chunk->read_task)
			scsi_free_scsi_task(chunk->read_task);
		chunk = next;
	}
}

void free_surplus_unused_chunks(struct input_st *input)
{
	unsigned maxreqs;
	struct chunk_st *chunk;

	/* Free $input->unused until $input->nunused drops to $maxreqs. */
	maxreqs = input->src->maxreqs + input->dst->maxreqs;
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
		printf("%s target: number of maximal outstanding requests "
		       "reduced to %u\n", which, endp->maxreqs);
}

void return_chunk(struct chunk_st *chunk)
{
	struct input_st *input = chunk->input;

	chunk->next = input->unused;
	input->unused = chunk;
	input->nunused++;
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

void create_chunks(struct input_st *input, unsigned nchunks)
{
	size_t inline_buf_size;

	/*
	 * If we're copying a local file to a remote target the input buffer
	 * is allocated together with the $chunk, the size of which is the
	 * destination device's blocksize.  In this case we need to discount
	 * the space allocated for the $read_task pointer, because its size
	 * is included in the size of the structure.
	 */
	assert(!LOCAL_TO_REMOTE(input)
		|| input->dst->blocksize > sizeof(struct scsi_task *));
	inline_buf_size = LOCAL_TO_REMOTE(input)
		? input->dst->blocksize - sizeof(struct scsi_task *): 0;
	for (; nchunks > 0; nchunks--)
	{
		struct chunk_st *chunk;

		chunk = xcalloc(sizeof(*chunk) + inline_buf_size);
		chunk->input = input;
		return_chunk(chunk);
	} /* until $nchunks unused chunks are created */
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
	} else	/* This is actually $endp->fname. */
		endp->url = NULL;

	if (endp->url)
	{
		iscsi_destroy_url(endp->url);
		endp->url = NULL;
	}
}

int init_endpoint(struct endpoint_st *endp, char const *which,
	char const *initiator, char const *url, int is_file)
{
	struct scsi_task *task;
	struct scsi_readcapacity10 *cap;
	struct scsi_inquiry_block_limits *inq;

	if (is_file)
	{	/* Endpoint is local, device charactersitic is irrelevant. */
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
	}

	/* Get the endpoint's nblocks and blocksize. */
	if (!(task = iscsi_readcapacity10_sync(endp->iscsi, endp->url->lun,
		0, 0)))
	{
		warn_iscsi("readcapacity10", endp->iscsi);
		destroy_endpoint(endp);
		return 0;
	} else if (task->status != SCSI_STATUS_GOOD
		|| !(cap = scsi_datain_unmarshall(task)))
	{
		warn_errno("readcapacity10");
		destroy_endpoint(endp);
		return 0;
	}

	/* We need a useful blocksize.  I think 512 byte blocks
	 * should be supported by all SCSI devices. */
	endp->blocksize = cap->block_size;
	if (endp->blocksize < 512)
	{
		warn("%s target reported blocksize=0, ignored\n", which);
		endp->blocksize = 512;
	}
	endp->nblocks = cap->lba + 1;
	scsi_free_scsi_task(task);

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

	if (Opt_verbosity > 0)
		printf("%s target: blocksize=%u, nblocks=%lu\n",
			which, endp->blocksize, endp->nblocks);

	return 1;
}

int local_to_remote(char const *initiator, struct input_st *input)
{
	int eof;
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

	eof = 0;
	pfd[1].fd = iscsi_get_fd(dst->iscsi);
	for (;;)
	{
		restart_requests(input);
		if (eof && !input->output->nreqs && !input->failed)
			break;

		pfd[0].events = !eof && input->unused ? POLLIN : 0;
		pfd[1].events = iscsi_which_events(dst->iscsi);
		if (!xfpoll(pfd, MEMBS_OF(pfd), input))
			continue;

		if (pfd[0].revents)
		{
			size_t n;
			struct chunk_st *chunk;

			assert(!eof);
			chunk = input->unused;
			assert(chunk != NULL);
			if (!xread(pfd[0].fd, chunk->buf, dst->blocksize, &n))
			{
				warn_errno(src->fname
					? src->fname : "(stdin)");
				return 0;
			}

			/* Have we read less than expected? */
			if (n < dst->blocksize)
				eof = 1;

			if (n > 0)
			{	/* Remove $chunk from $input->unused. */
				input->nunused--;
				input->unused = chunk->next;
				chunk->next = NULL;
				chunk->srcblock = input->top_block++;

				assert(n <= dst->blocksize);
				if (n < dst->blocksize)
				{
					warn("source block %lu "
						"padded with zeroes",
						chunk->srcblock);
					memset(&chunk->buf[n], 0,
						dst->blocksize - n);
				}

				if (!iscsi_write10_task(
					dst->iscsi, dst->url->lun,
					chunk->buf, dst->blocksize,
					chunk->srcblock, 0, 0,
					dst->blocksize, chunk_written, chunk))
				{
					warn_iscsi("write10", dst->iscsi);
					die(NULL);
				} else
					input->output->nreqs++;
			}
		}

		if (!is_connection_error(dst->iscsi, "destination",
			pfd[1].revents))
		{
			run_iscsi_event_loop(dst->iscsi, pfd[1].revents);
			free_surplus_unused_chunks(input);
		} else if (reconnect_endpoint(dst, initiator))
		{
			reduce_maxreqs(dst, "destination");
			free_surplus_unused_chunks(input);
		} else
			return 0;
	} /* until $eof is reached and everything is written out */

	/* Close the input file if we opened it. */
	if (!src->fname)
		close(pfd[0].fd);

	return 1;
}

int remote_to_local(char const *initiator, struct input_st *input,
	unsigned output_flags)
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
		int eof;

		restart_requests(input);
		start_iscsi_read_requests(input);
		eof = !input->nreqs && !input->failed;
		if (eof && !input->output->enqueued)
			break;

		pfd[0].events = iscsi_which_events(src->iscsi);
		pfd[1].events = process_output_queue(-1, dst, input->output,
					!eof)
			? POLLOUT : 0;
		if (!xfpoll(pfd, MEMBS_OF(pfd), input))
			continue;

		if (!is_connection_error(src->iscsi, "source",
				pfd[0].revents))
			run_iscsi_event_loop(src->iscsi, pfd[0].revents);
		else if (reconnect_endpoint(src, initiator))
		{
			reduce_maxreqs(src, "source");
			free_surplus_unused_chunks(input);
		} else
			return 0;

		if (pfd[1].revents)
		{
			process_output_queue(pfd[1].fd, dst, input->output,
				!eof);
			free_surplus_unused_chunks(input);
		}
	}

	/* Close the input file if we opened it. */
	if (!dst->fname)
		close(pfd[0].fd);

	return 1;
}

int remote_to_remote(char const *initiator, struct input_st *input)
{
	struct pollfd pfd[2];
	struct endpoint_st *src = input->src;
	struct endpoint_st *dst = input->dst;

	pfd[0].fd = iscsi_get_fd(src->iscsi);
	pfd[1].fd = iscsi_get_fd(dst->iscsi);
	for (;;)
	{
		restart_requests(input);
		start_iscsi_read_requests(input);

		if (!input->nreqs && !input->output->nreqs && !input->failed)
			break;

		pfd[0].events = iscsi_which_events(src->iscsi);
		pfd[1].events = iscsi_which_events(dst->iscsi);
		if (!xfpoll(pfd, MEMBS_OF(pfd), input))
			continue;

		if (!is_connection_error(src->iscsi, "source",
				pfd[0].revents))
			run_iscsi_event_loop(src->iscsi, pfd[0].revents);
		else if (reconnect_endpoint(src, initiator))
		{
			reduce_maxreqs(src, "source");
			free_surplus_unused_chunks(input);
		} else
			return 0;

		if (!is_connection_error(dst->iscsi, "destination",
			pfd[1].revents))
		{
			run_iscsi_event_loop(dst->iscsi, pfd[1].revents);
			free_surplus_unused_chunks(input);
		} else if (reconnect_endpoint(dst, initiator))
		{
			reduce_maxreqs(dst, "destination");
			free_surplus_unused_chunks(input);
		} else
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
		struct endpoint_st endp;
	} src, dst;
	int isok, optchar;
	unsigned output_flags;
	char const *initiator;
	struct input_st input;
	struct output_st output;

	/* Initialize diagnostics. */
	if ((Basename = strrchr(argv[0], '/')) != NULL)
		Basename++;
	else
		Basename = argv[0];
	setvbuf(stderr, NULL, _IOLBF, 0);

	/* Prepare our working area. */
	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	memset(&input, 0, sizeof(input));
	memset(&output, 0, sizeof(output));
	input.src = &src.endp;
	input.dst = &dst.endp;
	input.output = &output;

	/* Parse the command line */
	output_flags = O_EXCL;

	/* These defaults are used in --debug mode. */
	initiator = "jaccom";
	src.url = "iscsi://127.0.0.1/iqn.2014-07.net.nsn-net.timmy:omu/0";
	dst.url = "iscsi://127.0.0.1/iqn.2014-07.net.nsn-net.timmy:omu/1";
	if (argv[1] && !strcmp(argv[1], "--debug"))
	{
		argc--;
		argv++;
	} else
		src.url = dst.url = NULL;

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
			src.endp.maxreqs = atoi(optarg);
			break;
		case 'd':
			dst.url = optarg;
			break;
		case 'D':
			dst.is_file = 1;
			dst.fname = optarg;
			break;
		case 'M':
			dst.endp.maxreqs = atoi(optarg);
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
	{	/* LOCAL_TO_REMOTE() */
		src.is_file = 1;
	} else if (!dst.url)
	{	/* REMOTE_TO_LOCAL() */
		dst.is_file = 1;

		/* process_output_queue() needs the block size of the source
		 * in order to calculate the output offset. */
		dst.endp.blocksize = src.endp.blocksize;
	}

	/* Make sure we have sane settings. */
	if (!src.is_file && !src.endp.maxreqs)
		src.endp.maxreqs = DFLT_INITIAL_MAX_ISCSI_REQS;
	if (!dst.is_file && !dst.endp.maxreqs)
		dst.endp.maxreqs = DFLT_INITIAL_MAX_ISCSI_REQS;
	if (!Opt_min_output_batch)
		Opt_min_output_batch = 1;
	if (Opt_max_output_queue < Opt_min_output_batch)
		Opt_max_output_queue = Opt_min_output_batch;

	/* Init */
	signal(SIGPIPE, SIG_IGN);
	if (!init_endpoint(&src.endp, "source", initiator,
			src.url, src.is_file))
		die(NULL);
	if (!init_endpoint(&dst.endp, "destination", initiator,
			dst.url, dst.is_file))
		die(NULL);
	create_chunks(&input, src.endp.maxreqs + dst.endp.maxreqs);

	/* Run */
	if (LOCAL_TO_REMOTE(&input))
		/* This is the least problematic. */
		isok = local_to_remote(initiator, &input);
	else if (REMOTE_TO_LOCAL(&input))
	{	/* Allocate $output->iov and $output->tasks,
		 * which are only needed in this mode. */
		output.max = Opt_max_output_queue;
		output.iov = xmalloc(sizeof(*output.iov) * output.max);
		output.tasks = xmalloc(sizeof(*output.tasks) * output.max);
		isok = remote_to_local(initiator, &input, output_flags);
	} else
	{
		if (dst.endp.blocksize > src.endp.blocksize)
			die("source target's blocksize must be at least "
				"as large as the destination's");
		else if (src.endp.blocksize % dst.endp.blocksize)
			die("source target's blocksize must be a multiply "
				"of the destination's");
		isok = remote_to_remote(initiator, &input);
	}

	/* Done */
	if (isok)
	{	/* If we're not $isok, the libiscsi context may be
		 * in inconsistent state. */
   		   if (src.endp.iscsi)
			iscsi_logout_sync(src.endp.iscsi);
		if (dst.endp.iscsi)
			iscsi_logout_sync(dst.endp.iscsi);
	}

	/* Free resources */
	free_chunks(input.unused);
	free_chunks(input.failed);
	input.unused = input.failed = NULL;
	destroy_endpoint(&src.endp);
	destroy_endpoint(&dst.endp);

	return !isok;
}

/* vim: set foldmarker={{{,}}} foldmethod=marker: */
/* End of sexycat.c */
