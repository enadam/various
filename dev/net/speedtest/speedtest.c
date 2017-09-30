/*
 * speedtest.c -- measure the IPv4/IPv6 TCP/SCTP connection speed
 *
 * This program is used to measure the amount of application-level
 * messages and protocol layer packets that can be transmitted between
 * two computers through IPv4/IPv6 and TCP/SCTP within a given timeframe.
 * It needs to be run in two instances: one is the sender, the other is
 * the receiver of the messages.  Unless explicitly disabled, the client
 * (which is not necessarily the sender) sets up iptables rules to have
 * statistics about the protocol-layer transmission.  The test can be
 * conducted with several processes transmitting at the same time.
 *
 * Compilation:	link with -lsctp -lrt.
 * Usage:	First start the server, then start the client.
 *		Unless run with --no-iptables, the client must
 *		be executed with root privileges.
 *
 * Synopses:
 * Server mode:
 *   speedtest	<common-options>
 * Client mode:
 *   speedtest	<common-options> <destination> [--quiet]
 *		[{--overall-stats|--wide-stats|--no-stats|--no-iptables}]
 *
 * <destination> tells the client where to connect.  It has to be a network
 * address (DNS names are not supported).  It's always the client which
 * connect()s to the server, regardless whether it's the sending or the
 * receiving side.  Therefore the server should be be started earlier than
 * the client.  (Nevertheless, if the client cannot connect() it retries
 * every second.)
 *
 * <common-options> are:
 *		[-6] [-I=<interface>] [--sctp] [--port=<port-number>]
 *		[--sender[=<duration>][/<intersleep>]|--receiver]
 *		[--pause] [-n <nprocs>]
 *
 *   -6			Communicate through IPv6 rather than through IPv4.
 *			Specifying <interface> will probably be necessary.
 *   -I=<interface>	If transmitting over IPv6, the socket's scope will
 *			be set to the index of <interface>.  Otherwise this
 *			option is ignored.
 *   --sctp		Communicate through SCTP rather than TCP.
 *   --port=<port-num>	Specify the TCP/SCTP port to listen on (server mode)
 *			or to connect to (in client mode).  The default is
 *			DFLT_SRVPORT.
 *   --sender[=<duration>][/<intersleep>]
 *			Be the sender of the messages (default for clients)
 *			for <duration> seconds (a minute by default), and
 *			pausing for <intersleep> milliseconds between writes
 *			(default is none).
 *   --receiver		Be the receiver of the messages (default for servers).
 *			You may choose the client to be the receiver and
 *			the server to be the sender, but other configurations
 *			are not possible.
 *   --pause		Pause until <Enter> before closing the connections.
 *			It's meant for the --sender side to make it possible
 *			to capture system state before the connection closed.
 *   -n <nprocs>	Test with <nprocs> connections.  The default is one.
 *			On the server side <nprocs> additional processes will
 *			be created hand all connections will be handled
 *			simultanously.  On the client side just one process
 *			will handle all connections, because it reflects the
 *			real environment more closely.
 *   --quiet		Don't show the command line of external commands.
 *   --overall-stats	On the client side provide packet-level statistics
 *			about input/output traffic.  Not used on server-side.
 *			For clients this is the default statistics setting.
 *   --wide-stats	Likewise, but provide statistics per connection.
 *			Differs only from --overall-stats if <nprocs> > 1.
 *   --no-stats		Don't set up iptables rules to provide statistics.
 *   --no-iptables	Don't try to execute any iptables(1) command, not
 *			even to clean up them.  This is the only flag with
 *			which you can run this program as ordinary user.
 *
 * All parameters should be the same for the client and the server, except
 * for --send/--receive (which should be the opposite of each other) and -I.
 */

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <setjmp.h>

#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>

/* On which port to listen/connect to. */
#define DFLT_SRVPORT				1234
#define MYCHAIN					"lbsdia"

/* Stringify @str. */
#define Q(str)					QQ(str)
#define QQ(str)					#str

/* A kind of assert() that prints the @errno as well.  Useful for
 * testing the success of system calls.  @cond is evaluated only once. */
#define MUSTBE(cond)				\
do						\
{						\
	if (!(cond))				\
	{					\
		fprintf(stderr, "speedtest: %s: %m\n", #cond); \
		abort();			\
	}					\
} while (0)

/* Private variables */
/* The following variables need to be global because signal handlers
 * need them. */
/*
 * The thing passed between senders and receivers.  Each time @n is
 * incremented.  The rest of the buffer is left undefined and is not
 * used for anything.
 */
static union cntbuf_t
{
	unsigned n;
	char c[300];
} Buf;

/* The point to jump to if a signal is received. */
static jmp_buf Stack;

/* Program code */
/* Private functions */
/* Terminates the test, but doesn't quit the program right away. */
static void sigterm(int unused)
{
	longjmp(Stack, 1);
} /* sigterm */

/* Interrupt suck() if @Buf.n hasn't increased since the last call. */
static void sigalrm(int unused)
{
	static unsigned last_n;

	if (!Buf.n)
		return;
	if (last_n < Buf.n)
		last_n = Buf.n;
	else
		raise(SIGPIPE);
} /* sigalrm */

/*
 * Like execlp(), except that it optionally prints the command to be
 * executed and also like system() it waits for the completition of
 * the child and finally it checks whether the command's exit status
 * is acceptable (0 or any of the tolerated ones).  Expected statuses
 * are specified after the terminating NULL of the command line args,
 * and similarly need to be terminated with a 0.  Returns the exit code
 * of @cmd.
 */
static unsigned command(int keep_quiet, char const *cmd, ...)
{
	int status;
	pid_t child;
	unsigned nargs;
	va_list cmdline;
	char **args, *basename;

	/* Count the number of arguments. */
	va_start(cmdline, cmd);
	for (nargs = 1; va_arg(cmdline, char *) != NULL; nargs++)
		;
	va_end(cmdline);

	/* Allocate and initialize @args. */
	MUSTBE((args = malloc(sizeof(char *) * (nargs + 1))) != NULL);
	args[0] = (char *)cmd;
	if ((basename = strrchr(args[0], '/')) != NULL)
		args[0] = ++basename;

	/* Print @cmdline. */
	va_start(cmdline, cmd);
	if (keep_quiet)
	{
		for (nargs = 1; (args[nargs] = va_arg(cmdline, char *));
		     nargs++)
			;
	} else
	{
		fputs(args[0], stdout);
		for (nargs = 1; (args[nargs] = va_arg(cmdline, char *));
		     nargs++)
		{
			putchar(' ');
			fputs(args[nargs], stdout);
		}
		putchar('\n');
	}

	/* Execute @cmd. */
	MUSTBE((child = fork()) >= 0);
	if (child)
	{	/* Wait for its completition and check for errors. */
		MUSTBE(wait(&status) == child);
		assert(WIFEXITED(status));
		if (WEXITSTATUS(status) != 0)
		{
			unsigned expected;

			while ((expected = va_arg(cmdline, unsigned)) != 0)
				if (WEXITSTATUS(status) == expected)
					break;
			assert(expected != 0);
		}
		va_end(cmdline);
	} else
	{
		if (keep_quiet)
		{
			close(STDERR_FILENO);
			MUSTBE(open("/dev/null", O_WRONLY) == STDERR_FILENO);
		}
		MUSTBE(!execvp(cmd, args));
	}

	return WEXITSTATUS(status);
} /* command */

/* Wait until @nchildren has SIGSTOP:ped. */
static void wait_for_children_to_stop(unsigned nchildren)
{
	for (; nchildren > 0; nchildren--)
		MUSTBE(waitpid(-1, NULL, WUNTRACED) > 0);
} /* wait_for_children_to_stop */

/*
 * fork() and if @pgid is not 0 place @child in that process group.
 * Otherwise, if @pgid is 0, then let it be the process group and
 * place the child in that group.  Returns the process group the
 * child was placed in,
 */
static pid_t fork_and_setpgrp(pid_t pgid)
{
	pid_t child;

	MUSTBE((child = fork()) >= 0);
	if (!child)
		return 0;
	if (!pgid)
		pgid = child;
	MUSTBE(!setpgid(child, pgid));
	return pgid;
} /* fork_and_setpgrp */

/* read() @sbuf - @lbuf from @fd and return
 * the number of bytes filled in @buf. */
static void suck(int fd, union cntbuf_t *buf, size_t *lbufp)
{
	ssize_t red;

	/* We expect SIGPIPE kill us if read() returned < 0. */
	red = read(fd, &buf->c[*lbufp], sizeof(*buf) - *lbufp);
	if (red <= 0)
		raise(SIGPIPE);

	*lbufp += red;
	if (*lbufp >= sizeof(*buf))
		*lbufp = 0;
} /* suck */

/* Poll @pfd and suck() every file descriptor until eternity. */
static void __attribute__((noreturn))
suck_deep(int pfd, union cntbuf_t *buf, struct timespec *clock)
{
	for (;;)
	{
		unsigned i;
		int nevents;
		struct epoll_event events[128];

		/* Wait for up to 3 seconds once we've read something
		 * for the first time.  If we timeout assume trouble. */
		MUSTBE((nevents = epoll_wait(pfd, events,
			sizeof(events)/sizeof(events[0]),
			clock ? 3000 : -1)) >= 0);
		if (!nevents)
			raise(SIGPIPE);

		/* Record the time of the first suck. */
		if (clock)
		{
			puts("I. The show has started.");
			MUSTBE(!clock_gettime(CLOCK_MONOTONIC, clock));
			clock = NULL;
		}

		/* Process each event. */
		for (i = 0; i < nevents; i++)
		{	/* Read one @buf. */
			size_t total = 0;
			do
				suck(events[i].data.fd, buf, &total);
			while (total > 0);
		}
	} /* until eternity */
} /* suck_deep */

/* Send an SCTP_ABORT to the server to make it exit timely. */
static void sctp_abort(int sfd)
{
	struct linger linger_value;

	memset(&linger_value, 0, sizeof(linger_value));
	linger_value.l_onoff = 1;
	MUSTBE(setsockopt(sfd, SOL_SOCKET, SO_LINGER,
		&linger_value, sizeof(linger_value)) == 0);
	close(sfd);
} /* sctp_abort */

/* The main function */
int main(int argc, char const *argv[])
{
	pid_t pgid;
	void *shpage;
	int sfd, *sfds, pfd;
	volatile int be_quiet, do_pause, stats;
	volatile int is_boss, is_ipv6, is_client, is_sender;
	unsigned i, timeout, devidx, port;
	volatile unsigned proto, nprocs;
	struct timespec intersleep, start, finish;
	char const *device, *portstr;
	char const *iptables, *protostr, *destination;
	char const *direction, *dsaddr, *dsport, *sport;
	struct rlimit coresize;
	union
	{
		struct sockaddr sa;
		struct sockaddr_in ip4;
		struct sockaddr_in6 ip6;
		struct sockaddr_storage ss;
	} saddr;

	/* Parse the command line. */
	/* Need help? */
	if (argv[1] && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
	{
		printf("usage: %s "
			"[-6] [-I=<interface>] [--sctp] [--port=<srv-port>] "
			"[--sender[=<duration>][/<intersleep>]|--receiver] "
			"[--pause] [-n <nprocs>] [<destination> [--quiet] "
			"[{--overall-stats|--wide-stats|--no-stats"
			"|--no-iptables}]]\n",
			argv[0]);
		return 0;
	}

	/* Get @is_ipv6. */
	if (argv[1] && !strcmp(argv[1], "-6"))
	{
		is_ipv6 = 1;
		argv++;
	} else
		is_ipv6 = 0;

	/* Get @device. */
	if (argv[1] && !strncmp(argv[1], "-I=", 3))
	{
		device = &argv[1][3];
		argv++;
	} else
		device = NULL;

	/* Get @proto. */
	if (argv[1] && !strcmp(argv[1], "--sctp"))
	{
		proto    = IPPROTO_SCTP;
		protostr = "sctp";
		argv++;
	} else
	{
		proto    = IPPROTO_TCP;
		protostr = "tcp";
	}

	/* Get @port. */
	if (argv[1] && !strncmp(argv[1], "--port=", 7))
	{
		portstr	= &argv[1][7];
		port	= atoi(portstr);
		argv++;
	} else
	{
		port	= DFLT_SRVPORT;
		portstr	= Q(DFLT_SRVPORT);
	}

	/* Decide whether @is_sender.  If not specified explicitly,
	 * it will depend on whether we're @is_client. */
	timeout = 60;
	is_sender = -1;
	if (argv[1])
	{
		unsigned ms;

		ms = 0;
		if (!strcmp(argv[1], "--sender")
			|| sscanf(argv[1], "--sender=%u/%u", &timeout, &ms)
				== 2
			|| sscanf(argv[1], "--sender=%u", &timeout) == 1
			|| sscanf(argv[1], "--sender/%u", &ms) == 1)
		{
			is_sender = 1;
			intersleep.tv_sec = ms / 1000;
			ms -= intersleep.tv_sec * 1000;
			intersleep.tv_nsec = 1000000 * ms;
		} else if (!strcmp(argv[1], "--receiver"))
			is_sender = 0;

		if (is_sender >= 0)
			argv++;
	}

	/* @do_pause? */
	if ((do_pause = argv[1] && !strcmp(argv[1], "--pause")))
		argv++;
	else
		do_pause = 0;

	/* Get @nprocs. */
	if (argv[1] && !strcmp(argv[1], "-n"))
	{
		if (!argv[2])
		{
			fprintf(stderr, "%s: -n: "
				"required parameter missing\n",
				argv[0]);
			exit(1);
		}
		nprocs = atoi(argv[2]);
		argv += 2;
	} else
		nprocs = 1;

	/* @destination := where to connect to */
	if ((destination = argv[1]) != NULL)
		argv++;
	is_client = !!destination;
	if (is_sender < 0)
		is_sender = is_client;

	/* @be_quiet? */
	if ((be_quiet = argv[1] && !strcmp(argv[1], "--quiet")))
		argv++;
	else
		be_quiet = 0;

	/* Use iptables? */
	stats = 0;
	if (!is_client)
	{	/* We don't use iptables on server side. */
		sport = dsaddr = dsport = direction = iptables = NULL;
	} else
	{	/* iptables binary name and arguments. */
		iptables = is_ipv6 ? "ip6tables" : "iptables";
		if (is_sender)
		{
			direction = "OUTPUT";
			dsaddr    = "-d";
			dsport    = "--dport";
			sport     = "--sport";
		} else
		{
			direction = "INPUT";
			dsaddr    = "-s";
			dsport    = "--sport";
			sport     = "--dport";
		}

		/* --no-stats/--overall-stats/--wide-stats */
		stats = 1;
		if (argv[1])
		{
			if (!strcmp(argv[1], "--no-iptables"))
			{
				stats = -1;
				argv++;
			} else if (!strcmp(argv[1], "--no-stats"))
			{
				stats = 0;
				argv++;
			} else if (!strcmp(argv[1], "--overall-stats"))
			{
				stats = 1;
				argv++;
			} else if (!strcmp(argv[1], "--wide-stats"))
			{
				stats = 2;
				argv++;
			}
		}

		/* @nprocs == 1 => @stats <= 1 */
		if (nprocs == 1 && stats == 2)
			stats = 1;

		if (stats >= 0 && geteuid() != 0)
			fputs("You're not root, I'll probably fail.\n",
				stderr);
	} /* is_client */

	/* No more arguments are expected. */
	if (argv[1])
	{
		fprintf(stderr, "%s: unexpected argument\n", argv[1]);
		exit(1);
	}

	/* Don't dump core for assert() failures. */
	memset(&coresize, 0, sizeof(coresize));
	setrlimit(RLIMIT_CORE, &coresize);

	/* Create a socket. */
	MUSTBE((sfd = socket(is_ipv6 ? PF_INET6 : PF_INET, SOCK_STREAM,
		proto)) >= 0);

	/* Acquire scope_id (= @devidx) for sockaddr_in6. 
	 * No idea why @sfd is needed to query it. */
	if (is_ipv6 && device)
	{
		struct ifreq iface_info;

		memset(&iface_info, 0, sizeof(iface_info));
		strcpy(iface_info.ifr_name, device);
		MUSTBE(!ioctl(sfd, SIOCGIFINDEX, &iface_info));
		devidx = iface_info.ifr_ifindex;
	} else
		devidx = 0;

	/* Initialize @saddr. */
	if (!is_ipv6)
	{	/* Ordinary IPv4. */
		memset(&saddr.ip4, 0, sizeof(saddr.ip4));
		saddr.ip4.sin_family = AF_INET;
		if (destination)
			assert(inet_aton(destination, &saddr.ip4.sin_addr));
		saddr.ip4.sin_port = htons(port);
	} else
	{	/* IPv6 */
		memset(&saddr.ip6, 0, sizeof(saddr.ip6));
		saddr.ip6.sin6_family = AF_INET6;
		if (destination)
			assert(inet_pton(AF_INET6, destination,
				&saddr.ip6.sin6_addr) == 1);
		saddr.ip6.sin6_port = htons(port);
		saddr.ip6.sin6_scope_id = devidx;
	}

	/*
	 * connect()/accept() up to @nprocs and also add @iptables rules.
	 * If we have more than one connection on the server side, each
	 * will be handled in a separate process, and this process will
	 * be responsible for controlling them.  Otherwise we'll have only
	 * a single process (even if @nprocs > 1 on the client side), which
	 * handles multiple connections via epoll.  This is to emulate the
	 * real-world situation as close as possible.
	 */
	pfd = -1;
	pgid = 0;
	is_boss = 1;
	shpage = NULL;
	if (!is_client)
	{	/* Server */
		int cfd;
		int reuseaddr;

		/* bind() and listen() */
		reuseaddr = 1;
		MUSTBE(!setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR,
			&reuseaddr, sizeof(reuseaddr)));
		while (bind(sfd, &saddr.sa, sizeof(saddr)) < 0)
		{
			fprintf(stderr, "bind: %m\n");
			sleep(2);
		}
		MUSTBE(!listen(sfd, 100));

		/* fork() and accept() */
		sfds = NULL;
		for (i = 0; i < nprocs; i++)
		{
			/*
			 * If there're more than one server processes,
			 * place them into a separate process group,
			 * so the boss will be able to kill() them all
			 * at once without killing itself.  The @pgid
			 * is the @pid of the first process we fork().
			 */
			if (nprocs > 1)
			{
				/* @shpage is used to share the counter
				 * between child processess. */
				if (!shpage)
					MUSTBE((shpage = mmap(NULL,
						PAGE_SIZE,
						PROT_READ|PROT_WRITE,
						MAP_SHARED|MAP_ANONYMOUS,
						-1, 0)) != MAP_FAILED);
				if ((pgid = fork_and_setpgrp(pgid)) != 0)
					continue;
				is_boss = 0;
			}

			MUSTBE((cfd = accept(sfd, NULL, NULL)) >= 0);
			sfd = cfd;
			break;
		} /* for @nprocs */
	} else	/* @is_client */
	{
		/* Common iptables setup. */
		/* Clean up previous run of speedtest.
		 * iptables -D ... -j DROP */
		if (stats >= 0)
			command(be_quiet,
				iptables, "-D", direction, "-p", protostr,
				dsaddr, destination, dsport, portstr,
				"-j", "DROP", NULL, 1, 0);
		/* iptables -D ...; this and the following are mut. ex. */
		if (stats >= 0 && command(be_quiet,
				iptables, "-D", direction,
				"-p", protostr, dsaddr, destination,
				dsport, portstr, NULL, 1, 0))
			/* iptables -D ... -j MYCHAIN */
			command(be_quiet,
				iptables, "-D", direction, "-j", MYCHAIN,
				NULL, 1, 2, 0);
		if (stats >= 2)
		{	/* Create/flush MYCHAIN. */
			/* iptables -F MYCHAIN || iptables -N MYCHAIN */
			if (command(be_quiet,
				    iptables, "-F", MYCHAIN, NULL, 1, 0))
				command(be_quiet,
					iptables, "-N", MYCHAIN, NULL, 0);
			/* iptables -I ... -j MYCHAIN */
			command(be_quiet,
				iptables, "-I", direction, "-j", MYCHAIN,
				NULL, 0);
		}

		/* Open the connect()ions to @saddr. */
		if (nprocs > 1)
		{	/* Allocate @sfds. */
			MUSTBE((sfds = malloc(sizeof(*sfds) * nprocs)));
			sfds[0] = sfd;
			if (!is_sender)
				MUSTBE((pfd = epoll_create1(0)) >= 0);
		} else	/* We'll have a single @sfd. */
			sfds = &sfd;
		for (i = 0; i < nprocs; i++)
		{
			if (i > 0)
			{
				/* Create a new @sfd because we've used up 
				 * what we created a long ago. */
				MUSTBE((sfd = socket(is_ipv6
					? PF_INET6 : PF_INET, SOCK_STREAM,
					proto)) >= 0);
				sfds[i] = sfd;
			}

			/* connect() to @destination and start polling it */
			while (connect(sfd, &saddr.sa, sizeof(saddr)) < 0)
			{
				fprintf(stderr, "connect: %m\n");
				sleep(2);
			}
			if (!is_sender && nprocs > 1)
			{
				struct epoll_event ev;

				assert(pfd >= 0);
				memset(&ev, 0, sizeof(ev));
				ev.data.fd = sfd;
				ev.events = EPOLLIN;
				MUSTBE(!epoll_ctl(pfd, EPOLL_CTL_ADD, sfd,
							&ev));
			} /* add @sfd to @pfd */

			/* Add ipchains statistical rules. */
			if (stats == 1 && i == 0)
			{	/* Add a single rule for all connections. */
				command(be_quiet,
					iptables, "-I", direction,
					"-p", protostr, dsaddr, destination,
					dsport, portstr,
					NULL, 0);
			} else if (stats >= 2)
			{	/* Add connection-specific statistics rule
				 * to MYCHAIN. */
				socklen_t ssaddr;
				char save[sizeof(saddr)];
				char cltport[sizeof("65535")];

				/* @save @saddr because getsockname()
				 * will overwrite it, but we'll need it
				 * if nprocs > 1. */
				memcpy(save, &saddr, sizeof(saddr));

				/* Find out our source @cltport. */
				ssaddr = sizeof(saddr.ss);
				MUSTBE(!getsockname(sfd, &saddr.sa, &ssaddr));
				snprintf(cltport, sizeof(cltport), "%u",
					ntohs(saddr.sa.sa_family == AF_INET
						? saddr.ip4.sin_port
						: saddr.ip6.sin6_port));

				/* iptables -A MYCHAIN ... -j RETURN */
				command(be_quiet,
					iptables, "-A", MYCHAIN,
					"-p", protostr, dsaddr, destination,
					sport, cltport, dsport, portstr,
					"-j", "RETURN",
					NULL, 0);

				/* Restore @saddr from @save. */
				memcpy(&saddr, save, sizeof(saddr));
			} /* has iptables */
		} /* for @nprocs */
	} /* @is_client */ 

	/* SIGALRM and @timeout has no significance for non-boss servers. */
	signal(SIGPIPE, sigterm);
	if (is_boss)
	{	/* @is_client or server and boss */
		/* ^C or timeout => interrupt the test */
		signal(SIGINT, sigterm);
		signal(SIGTERM, SIG_IGN);
		signal(SIGALRM, sigterm);
	} else	/* server and not boss */
	{	/* Broken pipe or request from the boss => interrupt.
		 * Ignore SIGINT, though, only obey our boss. */
		signal(SIGINT, SIG_IGN);
		signal(SIGTERM, sigterm);
	}

	/* Break out if we receive one of the signals above. */
	if (!setjmp(Stack))
	{
		/* From now on only a signal can get out us from this "if",
		 * except for one case (server(n) receiver and boss). */
		/* Client(n) sender/receiver. */
		if (is_client && nprocs > 1)
		{
			if (is_sender)
			{	/* Generate traffic. */
				puts("II. The show has started.");
				MUSTBE(!clock_gettime(CLOCK_MONOTONIC,
					&start));
				alarm(timeout);
				for (;;)
				{
					write(sfds[Buf.n % nprocs],
						Buf.c, sizeof(Buf));
					Buf.n++;
					if (intersleep.tv_sec
							|| intersleep.tv_nsec)
						nanosleep(&intersleep, NULL);
				} /* generate traffic */
			} else	/* receiver, consume traffic */
				suck_deep(pfd, &Buf, &start);
		} /* client(n) sender/receiver */

		/*
		 * Handled:
		 * -- client(n) sender
		 * -- client(n) receiver
		 */
		if (is_sender)
		{
			if (!is_client && nprocs > 1)
			{	/* Server(n) sender */
				if (!is_boss)
				{
					struct timespec ts;

					/* Stop until we're given free way. */
					raise(SIGSTOP);

					/* Wait for a short, random duration
					 * (<= 1s) so that the workers don't
					 * run at the same time. */
					srand(time(NULL));
					ts.tv_sec = 0;
					ts.tv_nsec = rand();
					ts.tv_nsec *= 1000000000/RAND_MAX;
				} else /* @is_boss */
				{
					/* Wait until all senders have
					 * SIGSTOP:ped. */
					wait_for_children_to_stop(nprocs);

					/* Start all senders, then wait for
					 * SIGALRM or SIGINT, which will make
					 * us longjmp(). */
					puts("III. The show has started.");
					alarm(timeout);
					MUSTBE(!kill(-pgid, SIGCONT));
					for (;;)
						pause();
					/* The only way out is longjump(). */
				} /* @is_boss */
			} /* server(n) sender */

			/*
			 * Handled:
			 * -- client(n) sender
			 * -- client(n) receiver
			 * -- server(n) sender and boss
			 *
			 * To be handled:
			 * -- client(1) sender
			 * -- server(1) sender
			 * -- server(n) sender and not boss
			 *
			 * -- client(1) receiver
			 * -- server(1) receiver
			 * -- server(n) receiver and boss
			 * -- server(n) receiver and not boss
			 */
			assert(is_client || nprocs == 1 || !is_boss);
			if (nprocs == 1)
			{
				puts("IV. The show has started.");
				alarm(timeout);
			}

			/* Generate traffic. */
			MUSTBE(!clock_gettime(CLOCK_MONOTONIC, &start));
			if (nprocs == 1)
			{	/* client(1) or server(1) */
				for (;;)
				{
					write(sfd, Buf.c, sizeof(Buf));
					Buf.n++;
					if (intersleep.tv_sec
							|| intersleep.tv_nsec)
						nanosleep(&intersleep, NULL);
				}
			} else	/* server(n) */
			{
				unsigned *ip;

				assert(!is_client && !is_boss);
				assert(shpage != NULL);
				ip = shpage;
				*ip = 0;
				for (;;)
				{
					write(sfd, Buf.c, sizeof(Buf));
					Buf.n = __sync_fetch_and_add(ip, 1);
					if (intersleep.tv_sec
							|| intersleep.tv_nsec)
						nanosleep(&intersleep, NULL);
				}
			} /* server(n) */
		} /* sender */

		/*
		 * Handled:
		 * -- client(1) sender
		 * -- client(n) sender
		 * -- client(n) receiver
		 * -- server(1) sender
		 * -- server(n) sender and boss
		 * -- server(n) sender and not boss
		 *
		 * To be handled:
		 * -- client(1) receiver
		 * -- server(1) receiver
		 * -- server(n) receiver and not boss
		 *
		 * -- server(n) receiver and boss
		 */
		assert(!is_sender);
		if (is_client || nprocs == 1 || !is_boss)
		{
			size_t total;
			struct pollfd pollst;

			/* Wait until the sender starts sending. */
			total = 0;
			pollst.fd = sfd;
			pollst.events = POLLIN;
			MUSTBE(poll(&pollst, 1, -1) == 1);
			if (nprocs == 1)
				puts("V. The show has started.");
			else if (__sync_bool_compare_and_swap(
					(unsigned *)(shpage + 4), 0, 1))
				puts("VI. The first show has started.");

			/* If no new data received in 3 seconds => trouble. */
			signal(SIGALRM, sigalrm);
			alarm(3);

			/* Go! */
			total = 0;
			MUSTBE(!clock_gettime(CLOCK_MONOTONIC, &start));
			for (;;) /* consume traffic */
				suck(sfd, &Buf, &total);
		} /* client(1)/server(1)/server(n) receiver */

		/* To be handled: server(n) receiver and boss */
	} else if (is_boss && !is_client && nprocs > 1)
		/* Indicate the server processes that the show is over. */
		MUSTBE(!kill(-pgid, SIGTERM));
	else /* @is_client || nprocs == 1 || !@is_boss */
		MUSTBE(!clock_gettime(CLOCK_MONOTONIC, &finish));

	if (stats > 0)
		/* @is_client: cut the connection. */
		command(be_quiet,
			iptables, "-I", direction, "-p", protostr,
			dsaddr, destination, dsport, portstr,
			"-j", "DROP", NULL, 0);

	if (is_client || nprocs == 1 || !is_boss)
	{	/* Print statistics and we're done. */
		unsigned elapsed;

		elapsed  = (finish.tv_sec  - start.tv_sec) * 1000;
		elapsed += (finish.tv_nsec - start.tv_nsec) / 1000000;
		printf("time: %u.%.4u,\tbuf.n: %u\n",
			elapsed / 1000, elapsed % 1000, Buf.n);

		/* Pause before we close the connection.  If we're !is_boss,
		 * exit. */
		if (!is_client)
		{
			/*
			 * If we need to pause, and we're not is_boss,
			 * then SIGSTOP ourselves.  The boss will wait
			 * until everyone has frozen then it prompts
			 * the user, and after that resumes execution.
			 */
			if (do_pause)
			{
				if (!is_boss)
					raise(SIGSTOP);
				else if (nprocs > 1)
					wait_for_children_to_stop(nprocs);

				/* Boss server: wait for user. */
				if (is_boss)
				{
					puts("Completed.  Hit <Enter>.");
					getchar();
					if (nprocs > 1)
						MUSTBE(!kill(-pgid, SIGCONT));
				}
			} /* do_pause */

			MUSTBE(!close(sfd));
			if (nprocs > 1)
			{
				assert(!is_boss);
				return 0;
			}
		} /* !is_client */
	} /* application level statistics */

	/* We're @is_boss from now on. */
	assert(is_boss);

	/* Wait for the children, so their output will appear first. */
	if (!is_client && nprocs > 1)
		for (i = 0; i < nprocs; i++)
			MUSTBE(wait(NULL) > 0);

	/* Print statistics and remove iptables rules. */
	if (stats > 0)
	{
		/* Our -j DROP rule moved the main statistical rule
		 * to the 2nd position. */
		command(be_quiet, iptables, "-v", "-x", "-n",
				"-L", direction, "2", NULL, 0);

		if (stats >= 2)
		{	/* Print per-connection statistics. */
			command(be_quiet, iptables, "-v", "-x", "-n",
				"-L", MYCHAIN, NULL, 0);

			/* Clean up MYCHAIN. */
			command(be_quiet, iptables, "-F", MYCHAIN, NULL, 0);
			command(be_quiet, iptables, "-D", direction,
				"-j", MYCHAIN, NULL, 0);
			command(be_quiet, iptables, "-X", MYCHAIN, NULL, 0);
		} else
			command(be_quiet,
				iptables, "-D", direction,
				"-p", protostr, dsaddr, destination,
				dsport, portstr,
				NULL, 0);

		/* Remove our DROP rule. */
		command(be_quiet, iptables, "-D", direction, "-p", protostr,
			dsaddr, destination, dsport, portstr,
			"-j", "DROP",
			NULL, 0);
	} /* statistics and iptables clean up */

	/* Client: wait for user. */
	if (do_pause && is_client)
	{
		puts("Completed.  Hit <Enter>.");
		getchar();
	}

	/* To remain in sync with the server ABORT the connections,
	 * and consequently make the server exit. */
	if (is_client && proto == IPPROTO_SCTP)
		for (i = 0; i < nprocs; i++)
			sctp_abort(sfds[i]);

	return 0;
} /* main */

/* End of speedtest.c */
