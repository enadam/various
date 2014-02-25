/*
 * sicktp.c -- SCTP swiss-army knife
 *
 * This program is meant to play with the SCTP protocol.  It embeds
 * both a client and a server, supports IPv4 and IPv6, multihoming
 * and SCTP notifications.
 *
 * Compilation: ``cc -Wall -lsctp sicktp.c -o sicktp''.
 *
 * Synopsis:
 *   sicktp [-46] [-p1|-p2] \
 *          {-s <port> <bind-addr> | -[dp] <port> <connect-addr>}...
 *          [-xX <program> [<arguments>]...]
 *
 * If left unspecified, IPv4 is assumed as network protocol.  This case
 * <bind-addr> and <connect-addr> must be IPv4 addresses.  If -6 is given,
 * both addresses can be either IPv4 or IPv6.  For IPv6 addresses an
 * optional "%<interface>" suffix is available, which tells `sicktp' the
 * scope id of the address (which is mandatory for link-local addresses).
 *
 * -p1 and -p2 selects the desired SCTP parameters; if none is specified,
 * no special SCTP setup is performed.
 *
 * You can list any number of addresses to bind to or to connect to.
 * If neither -[dp] is specified, server role is assumed and the program
 * listens on <bind-addr>:<bind-port>.  For clients it's possible to specify
 * both <bind-addr> and <connect-addr>.  If you don't want to choose the
 * client side <port> you can leave it 0.
 *
 * The difference between -d and -p is that in the latter case the following
 * IP address will be set primary with the SCTP_PRIMARY_ADDR socket option
 * when the connection comes up.
 *
 * With -x you can specify a program to launch when a connection is made
 * to a server or accepted from a client.  The program's standard output
 * will be redirected to/fro the connection.  -X is the same, except that
 * the standard input is redirected as well.
 *
 * There are probably many programs out there with similar functionality
 * as sicktp.  One key difference could be that this program was expressly
 * written for SCTP.
 */

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <string.h>
#include <stdio.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/sctp.h>
#include <net/if.h>

#include <sys/select.h>
#include <sys/ioctl.h>

/* Standard definitions */
/* SCTP parameters used by setup_sctp_default(). */
#define DIA_CONNECTION_T_CONN_COUNT_C	 500
#define DFLT_SINIT_MAX_ATTEMPTS		   4
#define DFLT_SINIT_MAX_INIT_TIMEO	2000
#define DFLT_SPP_HBINTERVAL		1000
#define DFLT_SRTO_INITIAL		 500
#define DFLT_SRTO_MIN			 500
#define DFLT_SRTO_MAX			1000

/* Type definitions */
/* Tightly packed pack of sockaddr_in:s and sockaddr_in6:es as buiilt
 * by add_addr() for the likings of sctp_bindx() and sctp_connectx(). */
struct addresses_st
{
	unsigned port, naddrs;
	size_t size;
	char saddr[];
};

/* Program code */
/* Utilities */
static void __attribute__((noreturn)) error_errno(char const *fun)
{
	fprintf(stderr, "%s: %m\n", fun);
	exit(1);
} /* error_errno */

static void __attribute__((noreturn)) error(char const *str1,
					    char const *str2)
{
	fprintf(stderr, "%s: %s\n", str1, str2);
	exit(1);
} /* error */

static void __attribute__((noreturn)) usage(void)
{
	error("usage", "sicktp [-4 | -6] [-p1 | -p2] "
	      "{{-s <bind-port> <bind-addr>[%<interface>]} | "
	      "{-[dp] <connect-port> <connect-addr>[%<interface>]...}}... "
	      "[-x <program> [<arguments>]...]");
} /* usage */

static void ensure_arg(void const *ptr)
{
	if (!ptr)
		usage();
} /* ensure_arg */

/* Parse $str as an integer and err out if there's any problem with it. */
static unsigned parse_int(char const *str)
{
	unsigned n;
	char *end;

	n = strtol(str, &end, 0);
	if (!*str || *end)
		usage();
	return n;
} /* parse_int */

/* Convert $ip and $port to a $saddr (either _in or _in6).  For IPv6,
 * also parse the %<interface> portion of $ip if exists. */
static void parse_addr(int sfd, struct sockaddr_storage *saddr,
		       unsigned ip_version, char const *ip, unsigned port)
{
	char const *dev;
	unsigned devidx;
	char *ip_wo_dev;
	struct sockaddr_in saddr4;
	struct sockaddr_in6 saddr6;

	/* Is there a %<interface> suffix? */
	if (ip_version == 6 && (dev = strchr(ip, '%')))
	{
		struct ifreq iface_info;

		/* Dup $ip and trim the %<inteface> part. */
		ip_wo_dev = strdup(ip);
		ip_wo_dev[dev - ip] = '\0';
		ip = ip_wo_dev;
		dev++;

		/* Get the interface index based on its name. */
		memset(&iface_info, 0, sizeof(iface_info));
		strcpy(iface_info.ifr_name, dev);
		assert(!ioctl(sfd, SIOCGIFINDEX, &iface_info));
		devidx = iface_info.ifr_ifindex;
	} else
	{	/* No <interface> specification. */
		ip_wo_dev = NULL;
		devidx = 0;
	}

	/* For IPv6, first try to parse $ip az an IPv6 address. */
	memset(&saddr6, 0, sizeof(saddr6));
	if (ip_version == 6
	    && inet_pton(AF_INET6, ip, &saddr6.sin6_addr) == 1)
	{
		saddr6.sin6_family = AF_INET6;
		saddr6.sin6_port = htons(port);
		saddr6.sin6_scope_id = devidx;
		memcpy(saddr, &saddr6, sizeof(saddr6));
		free(ip_wo_dev);
		return;
	}

	/* Parse $ip as an IPv4 address. */
	memset(&saddr4, 0, sizeof(saddr4));
	switch (inet_pton(AF_INET, ip, &saddr4.sin_addr))
	{
	case 0:
		error(ip, "not an IPv4 address");
	case -1: /* This should not happen. */
		error(ip, "wrong address family");
	default: /* Success */
		saddr4.sin_family = AF_INET;
		saddr4.sin_port = htons(port);
		memcpy(saddr, &saddr4, sizeof(saddr4));
		free(ip_wo_dev);
		return;
	} /* switch */
} /* parse_addr */

/* Add $saddr to $addresses either as a sockaddr_in or as a sockaddr_in6. */
static struct addresses_st *add_addr(struct addresses_st *addresses,
				     struct sockaddr_storage const *saddr)
{
	int init;
	size_t saddr_size, old_size, new_size;

	if (addresses)
	{
		init = 0;
		old_size = addresses->size;
	} else
	{
		init = 1;
		old_size = 0;
	}

	saddr_size = saddr->ss_family == AF_INET
		? sizeof(struct sockaddr_in)
		: sizeof(struct sockaddr_in6);

	new_size = old_size + saddr_size;
	assert((addresses = realloc(addresses,
				    sizeof(*addresses) + new_size)) != NULL);

	if (init)
		memset(addresses, 0, sizeof(*addresses));
	memcpy(&addresses->saddr[old_size], saddr, saddr_size);
	addresses->naddrs++;
	addresses->size = new_size;

	return addresses;
} /* add_addr */

/* Set the default SCTP parameters for $sfd. */
static void setup_sctp_default(int sfd)
{
	struct sctp_paddrparams paddr;
	struct sctp_initmsg initmsg;
	struct sctp_rtoinfo rto;

	memset(&initmsg, 0, sizeof(initmsg));
	initmsg.sinit_max_instreams  = DIA_CONNECTION_T_CONN_COUNT_C;
	initmsg.sinit_num_ostreams   = DIA_CONNECTION_T_CONN_COUNT_C;
	initmsg.sinit_max_attempts   = DFLT_SINIT_MAX_ATTEMPTS;
	initmsg.sinit_max_init_timeo = DFLT_SINIT_MAX_INIT_TIMEO;
	if (setsockopt(sfd, SOL_SCTP, SCTP_INITMSG,
		       &initmsg, sizeof(initmsg)) < 0)
		error_errno("setsockopt(SCTP_INITMSG)");

	memset(&paddr, 0, sizeof(paddr));
	paddr.spp_hbinterval = DFLT_SPP_HBINTERVAL;
	paddr.spp_flags      = SPP_HB_ENABLE;
	if (setsockopt(sfd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS,
		       &paddr, sizeof(paddr)) < 0) 
		error_errno("setsockopt(SCTP_PEER_ADDR_PARAMS)");

	memset(&rto, 0, sizeof(rto));
	rto.srto_initial = DFLT_SRTO_INITIAL;
	rto.srto_min     = DFLT_SRTO_MIN;
	rto.srto_max     = DFLT_SRTO_MAX;
	if (setsockopt(sfd, SOL_SCTP, SCTP_RTOINFO, &rto, sizeof(rto)) < 0)
		error_errno("setsockopt(SCTP_RTOINFO)");
} /* setup_sctp_default */

/* Set special SCTP parameters for $sfd. */
static void setup_sctp_special(int sfd)
{
	struct sctp_paddrparams paddr;
	struct sctp_rtoinfo rto;
	struct sctp_assocparams assoc;
	int nodelay;

	/* Applying HB interval, path max retransmission
	 * and SACK delay period parameters. */
	memset(&paddr, 0, sizeof(paddr));
	paddr.spp_hbinterval = 1000;
	paddr.spp_pathmaxrxt = 2;
	paddr.spp_sackdelay  = 110;
	paddr.spp_flags      = SPP_HB_ENABLE | SPP_SACKDELAY_ENABLE;
	if (setsockopt(sfd, SOL_SCTP, SCTP_PEER_ADDR_PARAMS, &paddr,
		       sizeof(paddr)) < 0)
	       	error_errno("setsockopt(SCTP_PEER_ADDR_PARAMS)");

	/* Applying RTO parameters. */
	memset(&rto, 0, sizeof(rto));
	rto.srto_initial = 200;
	rto.srto_min     = 150;
	rto.srto_max     = 200;
	if (setsockopt(sfd, SOL_SCTP, SCTP_RTOINFO, &rto, sizeof(rto)) < 0)
	       	error_errno("setsockopt(SCTP_RTOINFO)");

	/* Applying association max retransmission parameter. */
	memset(&assoc, 0, sizeof(assoc));
	assoc.sasoc_asocmaxrxt = 4;
	if (setsockopt(sfd, SOL_SCTP, SCTP_ASSOCINFO,
		       &assoc, sizeof(assoc)) < 0)
	       	error_errno("setsockopt(SCTP_ASSOCINFO)");

	/* Applying bundling parameter. */
	nodelay = 1;
	if (setsockopt(sfd, SOL_SCTP, SCTP_NODELAY,
		       &nodelay, sizeof(nodelay)) < 0)
	       	error_errno("setsockopt(SCTP_NODELAY)");
} /* setup_sctp_special */

/* Suck $sfd and print it if an SCTP notification arrived.  Otherwise,
 * silently throw it away.  If $primary is not NULL, it'll be set as
 * SCTP_PRIMARY_ADDR when SCTP_COMM_UP. */
static void read_sctp_notification(int sfd,
				   struct sockaddr_storage const *primary)
{
	int flags;
	char buf[1024];
	struct sctp_sndrcvinfo sinfo;
	const union sctp_notification *notif;

	/* Read into $buf and return if it's not a notification. */
	flags = MSG_DONTWAIT;
	if (sctp_recvmsg(sfd, buf, sizeof(buf), NULL, 0,
			 &sinfo, &flags) <= 0)
		return;
	if (!(flags & MSG_NOTIFICATION))
		return;

	/* Print SCTP_ASSOC_CHANGE and SCTP_PEER_ADDR_CHANGE. */
	notif = (const union sctp_notification *)&buf[0];
	fprintf(stderr, "notification %u\n", notif->sn_header.sn_type);
	switch (notif->sn_header.sn_type)
	{
	case SCTP_ASSOC_CHANGE:
		printf("SCTP_ASSOC_CHANGE: %u\n",
		       notif->sn_assoc_change.sac_state);
		if (notif->sn_assoc_change.sac_state == SCTP_COMM_UP
		    && primary)
		{
			struct sctp_setprim setprim;

			puts("setting SCTP_PRIMARY_ADDR");
			setprim.ssp_addr = *primary;
			if (setsockopt(sfd, SOL_SCTP, SCTP_PRIMARY_ADDR,
				       &setprim, sizeof(setprim)) < 0)
			       	error_errno("setsockopt(SCTP_PRIMARY_ADDR)");
		}
		break;
	case SCTP_PEER_ADDR_CHANGE: {
		char str[INET_ADDRSTRLEN];
		const struct sockaddr_in *saddr4 =
			(const struct sockaddr_in *)
			&notif->sn_paddr_change.spc_aaddr;
		inet_ntop(saddr4->sin_family, &saddr4->sin_addr,
			  str, INET_ADDRSTRLEN);
		sprintf(&str[0] + strlen(str), ":%u",
		       	ntohs(saddr4->sin_port));

		printf("SCTP_PEER_ADDR_CHANGE: %u (%s)\n",
		       notif->sn_paddr_change.spc_state, str);
		break;
	} /* case */
	} /* switch */
} /* sctp_notification */

/* Make $fd the stdout and optionally stdin, and exec($prog).
 * Leave stderr as it is. */
static void launch(int fd, int redir_stdin, char const *const *prog)
{
	if (redir_stdin && fd != STDIN_FILENO)
		assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
	if (fd != STDOUT_FILENO)
		assert(dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);

	/* Close $fd if it's not stdin/out/err. */
	if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO)
		close(fd);

	if (execvp(prog[0], (char *const *)prog) < 0)
		error(prog[0], strerror(errno));
} /* launch */

/* The main function */
int main(int argc, char const *argv[])
{
	int sfd;
	char what;
	int capital_ex;
	char const *const *prog;
	unsigned i, ip_version, port;
	struct addresses_st *src, *dst;
	struct sockaddr_storage primary;

	/* Parse the command line. */
	ip_version = 4;
	src = dst = NULL;

	/* -[46]? */
	i = 1;
	ensure_arg(argv[i]);
	if (!strcmp(argv[i], "-4"))
	{
		ip_version = 4;
		i++;
	} else if (!strcmp(argv[i], "-6"))
	{
		ip_version = 6;
		i++;
	}

	/* Create and set up $sfd. */
	if ((sfd = socket(ip_version == 4 ? PF_INET : PF_INET6, SOCK_STREAM,
			  IPPROTO_SCTP)) < 0)
		error_errno("socket");

	/* -p1, -p2 */
	if (argv[i] && !strcmp(argv[i], "-p1"))
	{
		setup_sctp_default(sfd);
		i++;
	} else if (argv[i] && !strcmp(argv[i], "-p2"))
	{
		setup_sctp_special(sfd);
		i++;
	}

	/* -[sdp] */
	port = 0;
	what = '\0';
	prog = NULL;
	primary.ss_family = AF_UNSPEC;
	ensure_arg(argv[i]);
	do
	{
		int make_primary;
		struct sockaddr_storage saddr;

		make_primary = 0;
		if (!strcmp(argv[i], "-s"))
		{
			i++;
			what = 's';
			port = parse_int(argv[i++]);
		} else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "-p"))
		{
			what = 'd';
			make_primary = argv[i++][1] == 'p';
			port = parse_int(argv[i++]);
		} else if (!strcmp(argv[i], "-x") || !strcmp(argv[i], "-X"))
		{
			capital_ex = argv[i][1] == 'X';
			if (!(prog = &argv[++i]))
				usage();
			break;
		} else if (!what)
			usage();

		ensure_arg(argv[i]);
		parse_addr(sfd, &saddr, ip_version, argv[i++], port);
		if (make_primary)
			primary = saddr;
		if (what == 's')
			src = add_addr(src, &saddr);
		else
			dst = add_addr(dst, &saddr);
	} while (argv[i]);

	/* Bind if -s <port> <bind-address>:es were specified. */
	if (src && sctp_bindx(sfd,
			      (struct sockaddr *)src->saddr, src->naddrs,
			      SCTP_BINDX_ADD_ADDR) < 0)
		error_errno("sctp_bindx()");

	/* Roll the drums. */
	if (dst)
	{	/* Client mode */
		if (!prog) {
			struct sctp_event_subscribe events;

			/* Tell the SCTP stack which events
			 * we're interested in. */
			memset(&events, 0, sizeof(events));
			events.sctp_association_event = 1;
			events.sctp_address_event = 1;
			events.sctp_shutdown_event = 1;
			if (setsockopt(sfd, SOL_SCTP, SCTP_EVENTS,
				       &events, sizeof(events)) < 0)
				error_errno("setsockopt(SCTP_EVENTS)");
		} // subscribe SCTP events

		/* Connect to $dst. */
		if (sctp_connectx(sfd,
				  (struct sockaddr *)dst->saddr, dst->naddrs,
				  NULL) < 0)
			error_errno("sctp_connectx()");

		// Do we have a $prog:ram to execute with $sfd as stdin/out?
		if (prog)
			// launch() doesn't return.
			launch(sfd, capital_ex, prog);

		/* Read the terminal and send it to the server until EOF. */
		for (;;)
		{
			fd_set fds;
			char line[128];

			/* Print the prompt. */
			write(STDOUT_FILENO, "> ", 2);

again:			/* Process SCTP events coming from $sfd. */
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds);
			FD_SET(sfd, &fds);
			select(sfd + 1, &fds, NULL, NULL, NULL);
			if (FD_ISSET(sfd, &fds))
			{
				read_sctp_notification(sfd,
					primary.ss_family == AF_UNSPEC
					? NULL : &primary);
				goto again;
			} /* read event on $sfd */

			/* Read the terminal and send it to the server. */
			if (!fgets(line, sizeof(line), stdin))
				break;
			write(sfd, line, strlen(line));
		} /* forever */
	} else
	{	/* Server mode.  Accept connections until forever. */
		assert(!listen(sfd, 1));
		signal(SIGCHLD, SIG_IGN);
		for (;;)
		{
			int cfd, len;
			char line[128];

			assert((cfd = accept(sfd, NULL, NULL)) >= 0);

			/* Fork a child and launch $prog if specified. */
			if (prog)
			{
				pid_t pid;

				pid = fork();
				if (!pid)
					launch(cfd, capital_ex, prog);
				assert(pid > 0);
				continue;
			}

			/* Read $cfd and print it until EOF. */
			while ((len = read(cfd, line, sizeof(line))) > 0)
			{
				line[len] = '\0';
				printf("< %s", line);
			}
			close(cfd);
		} /* forever */
	} /* client/server */

	/* Done */
	return 0;
} /* main */

/* End of sicktp.c */
