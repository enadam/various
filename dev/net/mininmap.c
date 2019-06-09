/*
 * mininmap.c -- unobtrusive TCP and SCTP port scanner
 *
 * This program sends TCP SYN or SCTP INIT probes to the specified ports
 * of a target host and based on the reply determines whether the port is
 * open or closed, or there's a timeout or an ICMP error.  Ports are scanned
 * in parallel at a configurable rate.  Both IPv4 and IPv6 are supported.
 *
 * Compilation: cc -Wall -lrt mininmap.c
 *
 * See `mininmap --help' or "The program's help text" in this source code
 * for the invocation and all options.  To run the CAP_NET_RAW capability
 * is required.
 */

/* Required for ppoll(2). */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

/* Include files */
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <netdb.h>
#include <getopt.h>

#include <sys/time.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <linux/errqueue.h>

/* Standard definitions */
/* Number of nanoseconds in a second. */
#define NANOSECS	1000000000UL

/* Flags in a TCP segment. */
#define TCP_FIN		0x01
#define TCP_SYN		0x02
#define TCP_RST		0x04
#define TCP_PUSH	0x08
#define TCP_ACK		0x10
#define TCP_URG		0x20

/* SCTP chunk types */
#define SCTP_INIT	1
#define SCTP_INIT_ACK	2
#define SCTP_ABORT	6

/* Macros */
#define IMPOSSIBLE()	assert(0)

/* Type definitions */
typedef uint64_t nanosecs_t;

/* We use our struct instead of tcphdr from netinet/tcp.h because we need
 * the flags as a number when reporting a reply with unexpected flags. */
struct tcp_header_st
{
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t seq;
	uint32_t ack;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t reserved:4;
	uint8_t data_offset:4;
#elif __BYTE_ORDER == __BIG_ENDIAN
	uint8_t data_offset:4;
	uint8_t reserved:4;
#endif
	uint8_t flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgent_ptr;
} __attribute__((packed));

/* SCTP packet header */
struct sctp_header_st
{
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t verification_tag;
	uint32_t checksum;
} __attribute__((packed));

/* Common SCTP chunk header */
struct sctp_chunk_st
{
	uint8_t chunk_type;
	uint8_t flags;
	uint16_t chunk_length;
} __attribute__((packed));

/* SCTP INIT chunk */
struct sctp_init_st
{
	struct sctp_chunk_st common;
	uint32_t init_tag;
	uint32_t a_rwnd;
	uint16_t outbound_streams;
	uint16_t inbound_streams;
	uint32_t init_tsn;
} __attribute__((packed));

/* SCTP ABORT chunk with a cause code. */
struct sctp_abort_st
{
	struct sctp_chunk_st common;
	uint16_t cause_code;
	uint16_t cause_length;
} __attribute__((packed));

/* A probe packet to send.  Reused between probes. */
struct probe_packet_st
{
	int rawfd;
	unsigned af, proto;
	struct sockaddr_storage src_addr, dst_addr;

	union
	{
		char buf[0];
		struct tcp_header_st tcp;
		struct __attribute__((packed))
		{
			struct sctp_header_st header;
			struct sctp_init_st init;
		} sctp;
	};
} __attribute__((packed));

/* Describes a probe. */
struct probe_st
{
	/* File descriptor of the socket holding @src_port open. */
	int portholder;

	/* The initial sequence number of the TCP probe or the SCTP Initiate
	 * Tag.  Used to validate the target's reply. */
	unsigned verifier;

	/* The port to probe. */
	unsigned src_port, dst_port;

	/* Remaining nanoseconds until timeout.  Used to expire_probes(). */
	nanosecs_t timeout;

	/* Outstanding probes are kept in a linked list in @Probes. */
	struct probe_st *prev, *next;
};

/* A port range (inclusive) to scan. */
struct port_range_st
{
	unsigned port_from;
	unsigned port_until;
};

/* Constants */
/* Table to calculate an SCTP packet's checksum. */
static const uint32_t CRC32C_TABLE[] =
{
	0x00000000, 0xF26B8303, 0xE13B70F7, 0x1350F3F4,
	0xC79A971F, 0x35F1141C, 0x26A1E7E8, 0xD4CA64EB,
	0x8AD958CF, 0x78B2DBCC, 0x6BE22838, 0x9989AB3B,
	0x4D43CFD0, 0xBF284CD3, 0xAC78BF27, 0x5E133C24,
	0x105EC76F, 0xE235446C, 0xF165B798, 0x030E349B,
	0xD7C45070, 0x25AFD373, 0x36FF2087, 0xC494A384,
	0x9A879FA0, 0x68EC1CA3, 0x7BBCEF57, 0x89D76C54,
	0x5D1D08BF, 0xAF768BBC, 0xBC267848, 0x4E4DFB4B,
	0x20BD8EDE, 0xD2D60DDD, 0xC186FE29, 0x33ED7D2A,
	0xE72719C1, 0x154C9AC2, 0x061C6936, 0xF477EA35,
	0xAA64D611, 0x580F5512, 0x4B5FA6E6, 0xB93425E5,
	0x6DFE410E, 0x9F95C20D, 0x8CC531F9, 0x7EAEB2FA,
	0x30E349B1, 0xC288CAB2, 0xD1D83946, 0x23B3BA45,
	0xF779DEAE, 0x05125DAD, 0x1642AE59, 0xE4292D5A,
	0xBA3A117E, 0x4851927D, 0x5B016189, 0xA96AE28A,
	0x7DA08661, 0x8FCB0562, 0x9C9BF696, 0x6EF07595,
	0x417B1DBC, 0xB3109EBF, 0xA0406D4B, 0x522BEE48,
	0x86E18AA3, 0x748A09A0, 0x67DAFA54, 0x95B17957,
	0xCBA24573, 0x39C9C670, 0x2A993584, 0xD8F2B687,
	0x0C38D26C, 0xFE53516F, 0xED03A29B, 0x1F682198,
	0x5125DAD3, 0xA34E59D0, 0xB01EAA24, 0x42752927,
	0x96BF4DCC, 0x64D4CECF, 0x77843D3B, 0x85EFBE38,
	0xDBFC821C, 0x2997011F, 0x3AC7F2EB, 0xC8AC71E8,
	0x1C661503, 0xEE0D9600, 0xFD5D65F4, 0x0F36E6F7,
	0x61C69362, 0x93AD1061, 0x80FDE395, 0x72966096,
	0xA65C047D, 0x5437877E, 0x4767748A, 0xB50CF789,
	0xEB1FCBAD, 0x197448AE, 0x0A24BB5A, 0xF84F3859,
	0x2C855CB2, 0xDEEEDFB1, 0xCDBE2C45, 0x3FD5AF46,
	0x7198540D, 0x83F3D70E, 0x90A324FA, 0x62C8A7F9,
	0xB602C312, 0x44694011, 0x5739B3E5, 0xA55230E6,
	0xFB410CC2, 0x092A8FC1, 0x1A7A7C35, 0xE811FF36,
	0x3CDB9BDD, 0xCEB018DE, 0xDDE0EB2A, 0x2F8B6829,
	0x82F63B78, 0x709DB87B, 0x63CD4B8F, 0x91A6C88C,
	0x456CAC67, 0xB7072F64, 0xA457DC90, 0x563C5F93,
	0x082F63B7, 0xFA44E0B4, 0xE9141340, 0x1B7F9043,
	0xCFB5F4A8, 0x3DDE77AB, 0x2E8E845F, 0xDCE5075C,
	0x92A8FC17, 0x60C37F14, 0x73938CE0, 0x81F80FE3,
	0x55326B08, 0xA759E80B, 0xB4091BFF, 0x466298FC,
	0x1871A4D8, 0xEA1A27DB, 0xF94AD42F, 0x0B21572C,
	0xDFEB33C7, 0x2D80B0C4, 0x3ED04330, 0xCCBBC033,
	0xA24BB5A6, 0x502036A5, 0x4370C551, 0xB11B4652,
	0x65D122B9, 0x97BAA1BA, 0x84EA524E, 0x7681D14D,
	0x2892ED69, 0xDAF96E6A, 0xC9A99D9E, 0x3BC21E9D,
	0xEF087A76, 0x1D63F975, 0x0E330A81, 0xFC588982,
	0xB21572C9, 0x407EF1CA, 0x532E023E, 0xA145813D,
	0x758FE5D6, 0x87E466D5, 0x94B49521, 0x66DF1622,
	0x38CC2A06, 0xCAA7A905, 0xD9F75AF1, 0x2B9CD9F2,
	0xFF56BD19, 0x0D3D3E1A, 0x1E6DCDEE, 0xEC064EED,
	0xC38D26C4, 0x31E6A5C7, 0x22B65633, 0xD0DDD530,
	0x0417B1DB, 0xF67C32D8, 0xE52CC12C, 0x1747422F,
	0x49547E0B, 0xBB3FFD08, 0xA86F0EFC, 0x5A048DFF,
	0x8ECEE914, 0x7CA56A17, 0x6FF599E3, 0x9D9E1AE0,
	0xD3D3E1AB, 0x21B862A8, 0x32E8915C, 0xC083125F,
	0x144976B4, 0xE622F5B7, 0xF5720643, 0x07198540,
	0x590AB964, 0xAB613A67, 0xB831C993, 0x4A5A4A90,
	0x9E902E7B, 0x6CFBAD78, 0x7FAB5E8C, 0x8DC0DD8F,
	0xE330A81A, 0x115B2B19, 0x020BD8ED, 0xF0605BEE,
	0x24AA3F05, 0xD6C1BC06, 0xC5914FF2, 0x37FACCF1,
	0x69E9F0D5, 0x9B8273D6, 0x88D28022, 0x7AB90321,
	0xAE7367CA, 0x5C18E4C9, 0x4F48173D, 0xBD23943E,
	0xF36E6F75, 0x0105EC76, 0x12551F82, 0xE03E9C81,
	0x34F4F86A, 0xC69F7B69, 0xD5CF889D, 0x27A40B9E,
	0x79B737BA, 0x8BDCB4B9, 0x988C474D, 0x6AE7C44E,
	0xBE2DA0A5, 0x4C4623A6, 0x5F16D052, 0xAD7D5351,
};

/* Command line options. */
static struct option const Options[] =
{
	{ "help",		no_argument,		NULL, 'h' },
	{ "verbose",		no_argument,		NULL, 'v' },
	{ "quiet",		no_argument,		NULL, 'q' },
	{ "stats",		no_argument,		NULL, 's' },
	{ "ipv4",		no_argument,		NULL, '4' },
	{ "ipv6",		no_argument,		NULL, '6' },
	{ "sctp",		no_argument,		NULL, 'S' },
	{ "interface",		required_argument,	NULL, 'I' },
	{ "tos",		required_argument,	NULL, 'T' },
	{ "tclass",		required_argument,	NULL, 'T' },
	{ "verify-cksum",	no_argument,		NULL, 'k' },
	{ "dont-verify-cksum",	no_argument,		NULL, 'K' },
	{ "ppm",		required_argument,	NULL, 'n' },
	{ "timeout",		required_argument,	NULL, 't' },
	{ "all",		no_argument,		NULL, 'a' },
	{ "none",		no_argument,		NULL, 'N' },
	{ "open",		no_argument,		NULL, 'O' },
	{ "closed",		no_argument,		NULL, 'C' },
	{ "timed-out",		no_argument,		NULL, 'X' },
	{ NULL }
};

/* The program's help text. */
static char const Help[] =
"%s [<options>] <target> <ports>...\n"
"\n"
"Scan <target>'s TCP or SCTP <ports> unobtrusively.\n"
"\n"
"Options:\n"
"  -h, --help         Show this help.\n"
"  -v, --verbose      Print whenever a probe is sent.\n"
"  -q, --quiet        Do not print error replies (eg. ICMP).\n"
"  -s, --stats        Print statistics about the state of the scanned ports\n"
"                     at the end of the run.\n"
"\n"
"  -4, --ipv4         If <target> is a host name, scan its first IPv4 address.\n"
"  -6, --ipv6         Scan <target>'s first IPv6 address.  The default is\n"
"                     to scan the first resolved address regardless of its\n"
"                     family.\n"
"  -S, --sctp         Discover SCTP ports instead of TCP.\n"
"\n"
"  -I, --interface <NAME>\n"
"                     Send probes on this network interface.  Required if\n"
"                     <target> is an IPv6 link-local address.\n"
"  -T, --tos <TOS>, --tclass <TCLASS>\n"
"                     Set the Type-of-Service (IPv4) or Traffic-Class (IPv6)\n"
"                     field of the probes.  The argument must be a hexadecimal\n"
"                     number.\n"
"\n"
"  -k, --verify-cksum\n"
"  -K, --dont-verify-cksum\n"
"                     Verify or not the checksum of received packets before\n"
"                     processing.  Disabled for TCP by default because it\n"
"                     might not be calculated properly if the packet arrives\n"
"                     through a loopback-like interface.\n"
"\n"
"  -n, --ppm <PROBES-PER-MINUTE>\n"
"                     Send this number of probes a minute.  The default is 180.\n"
"  -t, --timeout <MILLISECONDS>\n"
"                     Wait for reply to a probe this much time.  The default is\n"
"                     three seconds.\n"
"\n"
"  -a, --all          Show the state of all the specified ports.  Same as -OCX.\n"
"  -O, --open         List definitely open ports.  This is the default.\n"
"  -C, --closed       List definitely closed ports.\n"
"  -X, --timed-out    List ports not sending reply until timeout.\n"
"                     These flags can be combined to override the default.\n"
"  -N, --none         Print none of the above.  Use `-s' to see statistics\n"
"                     instead.\n"
"\n"
"<target> can be IPv4 or IPv6 address or a host name.\n"
"<ports> can be service names, port numbers or port ranges (eg. 1-1024).\n"
"\n"
"The scanning is carried out unobtrusively, using TCP SYN or SCTP INIT probes.\n"
"The probes are sent asynchronously in parallel.\n";

/* Private variables */
/* The basename of the executable */
static char const *Prog;

/* Command line options */
static unsigned Opt_verbosity;
static nanosecs_t Opt_interval, Opt_timeout;
static int Opt_verify_checksum, Opt_statistics;
static int Opt_report_open, Opt_report_closed, Opt_report_timeout;

/* List of outstanding @Probes. */
static struct probe_st *Probes, *Last_probe;

/* Statistics */
unsigned Ports_open, Ports_closed, Ports_timed_out;
unsigned Error_sending, Error_responses, Unexpected_responses;

/* Program code */
/* Private functions */
/* Print an error message then die. */
static void __attribute__((noreturn, format(printf, 1, 2)))
fatal(char const *fmt, ...)
{
	int serrno;
	va_list args;

	serrno = errno;
	fprintf(stderr, "%s: ", Prog);

	errno = serrno;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fputc('\n', stderr);
	exit(1);
} /* fatal */

/* Report the state of @port. */
static void __attribute__((format(printf, 2, 3)))
report(unsigned port, char const *fmt, ...)
{
	int serrno;
	va_list args;

	serrno = errno;
	fprintf(stdout, "port %u: ", port);

	errno = serrno;
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);

	fputc('\n', stdout);
} /* report */

/* Return @str as an unsigned integer.  Unless @iserrp is provided,
 * a parse error is fatal().  Otherwise *@iserrp tells whether @str
 * could be parsed as an integer. */
static unsigned parse_integer(char const *str, int hexa, int *iserrp)
{
	int n;
	unsigned i;
	char const *fmt;

	i = 0;
	fmt = hexa ? "%x%n" : "%u%n";
	if (sscanf(str, fmt, &i, &n) < 1 || n != strlen(str))
	{	/* Parse error. */
		if (!iserrp)
			fatal("%s: invalid number", str);
		*iserrp = 1;
	} else if (iserrp)
		*iserrp = 0;
	return i;
} /* parse_integer */

/* Parse @str as a service name, single port number or a port range. */
static void parse_port_range(struct port_range_st *range, char const *str)
{
	int ret, n;

	/* Try to parse it as a port range. */
	ret = sscanf(str, "%u-%u%n",
			&range->port_from, &range->port_until, &n);
	if (ret < 2 || n != strlen(str))
	{	/* Pattern didn't match, assume it's a single port. */
		range->port_from = range->port_until =
			parse_integer(str, 0, &ret);
		if (ret)
		{	/* @str is not numeric, maybe it's a service name. */
			struct servent const *serv;

			if ((serv = getservbyname(str, NULL)) != NULL)
			{
				range->port_from = range->port_until =
					ntohs(serv->s_port);
				return;
			} else
				fatal("%s: unknown service", str);
		}
	}

	if (range->port_from > UINT16_MAX)
		fatal("%u: invalid port number", range->port_from);
	if (range->port_until > UINT16_MAX)
		fatal("%u: invalid port number", range->port_until);
} /* parse_port_range */

/* Return the difference between @after and @before in nanoseconds. */
static nanosecs_t timespec_diff(
	struct timespec const *before, struct timespec const *after)
{
	nanosecs_t diff;

	diff = (after->tv_sec - before->tv_sec) * NANOSECS;
	diff += after->tv_nsec - before->tv_nsec;
	return diff;
} /* timespec_diff */

/* Calculate the TCP checksum of @packet. */
static uint16_t tcp_checksum(struct probe_packet_st const *packet,
	char const *buf, size_t sbuf)
{
	union
	{	/* This pseudo IP header must be prepended to the TCP segment
		 * when calculating the checksum. */
		struct __attribute__((packed))
		{
			struct in_addr src_addr;
			struct in_addr dst_addr;
			uint8_t zero;
			uint8_t transport_protocol;
			uint16_t tcp_length;
		} ip4;

		struct __attribute__((packed))
		{
			struct in6_addr src_addr;
			struct in6_addr dst_addr;
			uint8_t zero;
			uint8_t transport_protocol;
			uint16_t tcp_length;
		} ip6;
	} pseudo_ip_hdr;
	size_t spiph;

	/* Fill in the @pseudo_ip_hdr. */
	switch (packet->af)
	{
	case AF_INET:
		spiph = sizeof(pseudo_ip_hdr.ip4);

		pseudo_ip_hdr.ip4.src_addr =
			((struct sockaddr_in const *)&packet->src_addr)
				->sin_addr;
		pseudo_ip_hdr.ip4.dst_addr =
			((struct sockaddr_in const *)&packet->dst_addr)
				->sin_addr;

		pseudo_ip_hdr.ip4.zero			= 0;
		pseudo_ip_hdr.ip4.transport_protocol	= packet->proto;
		pseudo_ip_hdr.ip4.tcp_length		= htons(sbuf);
		break;

	case AF_INET6:
		spiph = sizeof(pseudo_ip_hdr.ip6);

		pseudo_ip_hdr.ip6.src_addr =
			((struct sockaddr_in6 const *)&packet->src_addr)
				->sin6_addr;
		pseudo_ip_hdr.ip6.dst_addr =
			((struct sockaddr_in6 const *)&packet->dst_addr)
				->sin6_addr;

		pseudo_ip_hdr.ip6.zero 			= 0;
		pseudo_ip_hdr.ip6.transport_protocol	= packet->proto;
		pseudo_ip_hdr.ip6.tcp_length		= htons(sbuf);
		break;

	default:
		IMPOSSIBLE();
	}

	size_t nw;
        uint16_t const *wp;
        unsigned sum;

	sum = 0;

	assert(spiph % 2 == 0);
	wp = (uint16_t const *)&pseudo_ip_hdr;
	for (nw = spiph / 2; nw > 0; nw--)
		sum += *wp++;

        for (; sbuf >= 2; buf += 2, sbuf -= 2)
		sum += *(uint16_t const *)buf;
	if (sbuf > 0)
		sum += htons(*(uint8_t const *)buf << 8);

        sum = (sum >> 16) + (sum & 0xffff);
        sum += (sum >> 16);
	return ~sum;
} /* tcp_checksum */

static uint32_t crc32c(uint32_t crc, uint8_t const *buf, unsigned sbuf)
{
	while (sbuf--)
		crc = CRC32C_TABLE[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
	return crc;
} /* crc32c */

/* Calculate the CRC32C checksum of an SCTP packet. */
static uint32_t sctp_checksum(char const *buf, size_t sbuf)
{
	static const uint32_t zero;
	uint32_t checksum;

	/* Skip the last, checksum field of sctp_header_st. */
	assert(sbuf >= sizeof(struct sctp_header_st));
	checksum = crc32c(~(uint32_t)0,
		(void const *)buf,
		sizeof(struct sctp_header_st) - sizeof(zero));
	checksum = crc32c(checksum, (void const *)&zero, sizeof(zero));

	buf  += sizeof(struct sctp_header_st);
	sbuf -= sizeof(struct sctp_header_st);
	return ~crc32c(checksum, (void const *)buf, sbuf);
} /* sctp_checksum */

/* Unlink @probe and free all resources is holds. */
static void free_probe(struct probe_st *probe)
{
	if (probe->prev)
		probe->prev->next = probe->next;
	else if (Probes == probe)
		Probes = probe->next;
	if (probe->next)
		probe->next->prev = probe->prev;
	else if (Last_probe == probe)
		Last_probe = probe->prev;

	close(probe->portholder);
	free(probe);
} /* free_probe */

/* Go through the @Probes, update timeouts and remove expired ones. */
static void expire_probes(nanosecs_t elapsed)
{
	struct probe_st *probe;

	/* @Probes are ordered by timeout, so the expired ones come first. */
	while (Probes && Probes[0].timeout <= elapsed)
	{
		Ports_timed_out++;
		if (Opt_report_timeout)
			report(Probes[0].dst_port, "timeout");
		free_probe(Probes);
	}

	/* Subtract @elapsed from the @timeout of the remaining @Probes. */
	for (probe = Probes; probe; probe = probe->next)
	{
		assert(probe->timeout > elapsed);
		probe->timeout -= elapsed;
	}
} /* expire_probes */

/* Open the SOCK_RAW used to send probes. */
static void init_probe_socket(struct probe_packet_st *packet,
	char const *iface, unsigned tos)
{
	int val;
	socklen_t len;

	if ((packet->rawfd = socket(packet->af, SOCK_RAW|SOCK_NONBLOCK,
			packet->proto)) < 0)
		fatal("socket(SOCK_RAW): %m");

	if (iface&& setsockopt(packet->rawfd, SOL_SOCKET, SO_BINDTODEVICE,
			iface, strlen(iface) + 1) < 0)
		fatal("setsockopt(SO_BINDTODEVICE): %m");

	if (connect(packet->rawfd, (struct sockaddr const *)&packet->dst_addr,
			sizeof(packet->dst_addr)) < 0)
		fatal("connect(SOCK_RAW): %m");

	/* Source address to bind the portholder socket to. */
	len = sizeof(packet->src_addr);
	if (getsockname(packet->rawfd,
			(struct sockaddr *)&packet->src_addr, &len) < 0)
		fatal("getsockname(SOCK_RAW): %m");
	assert(packet->src_addr.ss_family == packet->dst_addr.ss_family);

	/* Clear the port number in the returned address, because in reality
	 * it's the protocol number @rawfd is bound to. */
	val = 1;
	switch (packet->dst_addr.ss_family)
	{
	case AF_INET:
		((struct sockaddr_in *)&packet->src_addr)->sin_port = 0;
		if (setsockopt(packet->rawfd, SOL_IP, IP_RECVERR,
				&val, sizeof(val)) < 0)
			fatal("setsockopt(IP_RECVERR): %m");
		if (tos && setsockopt(packet->rawfd, SOL_IP, IP_TOS,
				&tos, sizeof(tos)) < 0)
			fatal("setsockopt(IP_TOS): %m");
		break;
	case AF_INET6:
		((struct sockaddr_in6 *)&packet->src_addr)->sin6_port = 0;
		if (setsockopt(packet->rawfd, SOL_IPV6, IPV6_RECVERR,
				&val, sizeof(val)) < 0)
			fatal("setsockopt(IP_RECVERR): %m");
		if (tos && setsockopt(packet->rawfd, SOL_IPV6, IPV6_TCLASS,
				&tos, sizeof(tos)) < 0)
			fatal("setsockopt(IPV6_TCLASS): %m");
		break;
	default:
		IMPOSSIBLE();
	}
} /* init_probe_socket */

/* Set up protocol-specific constant fields of the probe @packet. */
static void init_probe_packet(struct probe_packet_st *packet)
{
	switch (packet->proto)
	{
	case IPPROTO_TCP:
		packet->tcp.flags = TCP_SYN;
		packet->tcp.data_offset =
			sizeof(packet->tcp) / sizeof(uint32_t);
		break;
	case IPPROTO_SCTP:
		packet->sctp.init.common.chunk_type = SCTP_INIT;
		packet->sctp.init.common.chunk_length =
			htons(sizeof(packet->sctp.init));
		packet->sctp.init.outbound_streams =
			packet->sctp.init.inbound_streams = htons(1);
		packet->sctp.init.a_rwnd = htonl(32768);
		break;
	default:
		IMPOSSIBLE();
	}
} /* init_probe_packet */

/* Allocate a probe_st and send a probe to @port. */
static struct probe_st *send_probe_packet(
	struct probe_packet_st *packet, unsigned port)
{
	int portholder;
	struct probe_st *probe;
	struct sockaddr_storage saddr;
	socklen_t len;

	if (!(probe = malloc(sizeof(*probe))))
	{
		report(port, "malloc(%zu): %m", sizeof(*probe));
		Error_sending++;
		return NULL;
	}

	memset(probe, 0, sizeof(*probe));
	while ((probe->verifier = rand()) == 0)
		/* The verifier can't be 0 for SCTP's sake. */;
	probe->timeout = Opt_timeout;

	if ((portholder = socket(packet->af, SOCK_STREAM, packet->proto)) < 0)
	{
		report(port, "socket(portholder): %m");
		Error_sending++;
		free(probe);
		return NULL;
	}
	probe->portholder = portholder;

	/* Bind the @portholder to a random local port. */
	if (bind(portholder,
		(struct sockaddr const *)&packet->src_addr,
		sizeof(saddr)) < 0)
	{
		report(port, "bind(portholder): %m");
		Error_sending++;
		free_probe(probe);
		return NULL;
	}

	/* @probe->src_port <- @portholder's local port number */
	len = sizeof(saddr);
	if (getsockname(portholder, (struct sockaddr *)&saddr, &len) < 0)
	{
		report(port, "getsockname(portholder): %m");
		Error_sending++;
		free_probe(probe);
		return NULL;
	}

	assert(saddr.ss_family == packet->af);
	probe->src_port = ntohs(saddr.ss_family == AF_INET
		? ((struct sockaddr_in const *)&saddr)->sin_port
		: ((struct sockaddr_in6 const *)&saddr)->sin6_port);
	probe->dst_port = port;

	/* Fill in the protocol-specific varying fields of the @packet. */
	switch (packet->proto)
	{
	case IPPROTO_TCP:
		len = sizeof(packet->tcp);
		packet->tcp.src_port = htons(probe->src_port);
		packet->tcp.dst_port = htons(probe->dst_port);
		packet->tcp.seq = htonl(probe->verifier);
		packet->tcp.checksum = 0;
		packet->tcp.checksum = tcp_checksum(packet,
			(void const *)&packet->tcp, sizeof(packet->tcp));
		break;
	case IPPROTO_SCTP:
		len = sizeof(packet->sctp);
		packet->sctp.header.src_port = htons(probe->src_port);
		packet->sctp.header.dst_port = htons(probe->dst_port);
		packet->sctp.init.init_tag = htonl(probe->verifier);
		packet->sctp.init.init_tsn = packet->sctp.init.init_tag;
		packet->sctp.header.checksum = sctp_checksum(
			(void const *)&packet->sctp, sizeof(packet->sctp));
		break;
	default:
		IMPOSSIBLE();
	}

	if (send(packet->rawfd, packet->buf, len, 0) < 0)
	{
		report(port, "send(SOCK_RAW): %m");
		Error_sending++;
		free_probe(probe);
		return NULL;
	}

	if (Opt_verbosity > 1)
		printf("probe sent: %u -> %u\n",
			probe->src_port, probe->dst_port);

	return probe;
} /* send_probe_packet */

/* Print the state of a TCP @port. */
static void handle_tcp_probe_reply(unsigned port,
	struct tcp_header_st const *tcp)
{
	if (tcp->flags == (TCP_SYN|TCP_ACK))
	{
		Ports_open++;
		if (Opt_report_open)
			report(port, "open");
	} else if (tcp->flags & TCP_RST)
	{
		Ports_closed++;
		if (Opt_report_closed)
			report(port, "closed");
	} else
	{
		Unexpected_responses++;
		if (Opt_verbosity > 0)
			report(port, "unexpected response (flags: 0x%x)",
				tcp->flags);
	}
} /* handle_tcp_probe_reply */

/* Print the state of an SCTP @port. */
static void handle_sctp_probe_reply(unsigned port,
	char const *buf, size_t sbuf)
{
	assert(sbuf >= sizeof(struct sctp_header_st));
	buf  += sizeof(struct sctp_header_st);
	sbuf -= sizeof(struct sctp_header_st);

	/* Go through all chunks in the response packet. */
	while (sbuf >= sizeof(struct sctp_chunk_st))
	{
		size_t chunk_length;
		struct sctp_chunk_st const *chunk;

		chunk = (void const *)buf;
		if (chunk->chunk_type == SCTP_INIT_ACK)
		{
			Ports_open++;
			if (Opt_report_open)
				report(port, "open");
			return;
		} else if (chunk->chunk_type == SCTP_ABORT)
		{
			struct sctp_abort_st const *err;

			Ports_closed++;
			err = (void const *)buf;
			if (!Opt_report_closed)
				/* NOP */;
			else if (sbuf >= sizeof(*err)
				 	&& sizeof(*err) <= ntohs(
						err->common.chunk_length))
				report(port, "closed (cause: %u)",
					ntohs(err->cause_code));
			else	/* No error cause. */
				report(port, "closed");
			return;
		}

		/* Advance @buf to the next chunk. */
		chunk_length = ntohs(chunk->chunk_length);
		if (sbuf < chunk_length)
			break;
		buf  += chunk_length;
		sbuf -= chunk_length;
	} /* for each chunk */

	Unexpected_responses++;
	if (Opt_verbosity > 0)
		report(port, "unexpected response");
} /* handle_sctp_probe_reply */

/* Print an IP_RECVERR/IPV6_RECVERR cmsg. */
static void handle_probe_error(unsigned port, struct msghdr *msg)
{
	struct cmsghdr *cmsg;

	Error_responses++;
	if (!Opt_verbosity)
		/* Not interested in errors. */
		return;

	/* Find the relevant @cmsg. */
	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg))
	{
		if (cmsg->cmsg_level == SOL_IP
				&& cmsg->cmsg_type == IP_RECVERR)
			break;
		if (cmsg->cmsg_level == SOL_IPV6
				&& cmsg->cmsg_type == IPV6_RECVERR)
			break;
	}

	if (cmsg)
	{
		struct sock_extended_err const *ee;

		ee = (void const *)CMSG_DATA(cmsg);
		if (ee->ee_origin == SO_EE_ORIGIN_LOCAL)
			report(port, "%s", strerror(ee->ee_errno));
		else if (ee->ee_origin == SO_EE_ORIGIN_ICMP
				|| ee->ee_origin == SO_EE_ORIGIN_ICMP6)
		{	/* We got an ICMP error. */
			char str[INET6_ADDRSTRLEN];
			struct sockaddr const *offender;
			size_t soffender;

			/* @offender should point right after @ee. */
			offender = SO_EE_OFFENDER(ee);
			assert(offender != NULL);

			if (offender->sa_family == AF_INET)
				soffender = sizeof(struct sockaddr_in);
			else if (offender->sa_family == AF_INET6)
				soffender = sizeof(struct sockaddr_in6);
			else
				soffender = 0;

			if (!soffender || getnameinfo(offender, soffender,
					str, sizeof(str), NULL, 0,
					NI_NUMERICHOST) != 0)
				str[0] = '\0';

			/* Print the OS error, the sender of the ICMP error,
			 * the message type and code. */
			report(port, "%s (%s: ICMP: %u/%u)",
				strerror(ee->ee_errno),
				str[0] ? str : "[unknown]",
				ee->ee_type, ee->ee_code);
		} else	/* Unknown @ee_origin. */
			report(port, "unknown error");
	} else	/* IP_RECVERR/IPV6_RECVERR cmsg not found. */
		report(port, "unknown error");
} /* handle_probe_error */

/* Find the probe @msg was sent in response to and process the reply.
 * Returns the matching probe_st or NULL. */
static struct probe_st *handle_probe_reply(
	struct probe_packet_st const *packet,
	struct msghdr *msg, char const *buf, size_t sbuf,
	int iserr)
{
	struct probe_st *probe;
	struct tcp_header_st const *tcp;
	struct sctp_header_st const *sctp;
	unsigned probe_src_port, probe_dst_port;

	if (packet->af == AF_INET && !iserr)
	{	/* The @ip header is included in @buf, skip it. */
		size_t siph;
		struct iphdr const *ip;

		if (sbuf < sizeof(*ip))
			return NULL;
		ip = (void const *)buf;
		assert(ip->protocol == packet->proto);

		siph = ip->ihl * sizeof(uint32_t);
		if (sbuf < siph)
			return NULL;
		buf  += siph;
		sbuf -= siph;
	}

	/* Verify the reply @msg's checksum if we have the full packet,
	 * then get the port numbers from the packet. */
	tcp = NULL;
	sctp = NULL;
	switch (packet->proto)
	{
	case IPPROTO_TCP:
		if (sbuf < sizeof(*tcp))
			return NULL;
		tcp = (void const *)buf;

		if (!iserr)
		{	/* The calculated checksum must be 0 invariantly. */
			if (Opt_verify_checksum
			    		&& !(msg->msg_flags & MSG_TRUNC))
				if (tcp_checksum(packet, buf, sbuf) != 0)
					return NULL;

			/* In the reply the port numbers are reflected. */
			probe_src_port = ntohs(tcp->dst_port);
			probe_dst_port = ntohs(tcp->src_port);
		} else
		{	/* @msg is the one we sent originally. */
			probe_src_port = ntohs(tcp->src_port);
			probe_dst_port = ntohs(tcp->dst_port);
		}
		break;

	case IPPROTO_SCTP:
		if (sbuf < sizeof(*sctp))
			return NULL;
		sctp = (void const *)buf;

		if (!iserr)
		{	/* On Linux the checksum is not calculated (left 0)
			 * if talking to localhost. */
			if (Opt_verify_checksum && sctp->checksum
			    		&& !(msg->msg_flags & MSG_TRUNC))
				if (sctp_checksum(buf, sbuf) != sctp->checksum)
					return NULL;

			probe_src_port = ntohs(sctp->dst_port);
			probe_dst_port = ntohs(sctp->src_port);
		} else
		{
			probe_src_port = ntohs(sctp->src_port);
			probe_dst_port = ntohs(sctp->dst_port);
		}
		break;

	default:
		IMPOSSIBLE();
	}

	/* Find the matching @probe by the port number. */
	for (probe = Probes; probe; probe = probe->next)
	{

		if (probe_src_port != probe->src_port)
			continue;
		if (probe_dst_port != probe->dst_port)
			/* Wrong port number. */
			return NULL;

		/* Process the @msg depending on the protocol. */
		if (iserr)
		{	/* Local or ICMP error. */
			handle_probe_error(probe_dst_port, msg);
		} else if (tcp)
		{
			if (ntohl(tcp->ack) != probe->verifier + 1)
				/* Wrong sequence number acknowledged. */
				return NULL;
			handle_tcp_probe_reply(probe_dst_port, tcp);
		} else	/* IPPROTO_SCTP */
		{
			assert(sctp != NULL);
			if (ntohl(sctp->verification_tag) != probe->verifier)
				/* Wrong Verification Tag. */
				return NULL;
			handle_sctp_probe_reply(probe_dst_port, buf, sbuf);
		}
		break;
	}

	return probe;
} /* handle_probe_reply */

/* recvmsg(@rawfd) then return handle_probe_reply().
 * @iserr indicates wheter the event received from @rawfd was a POLLERR. */
static struct probe_st *recv_probe_reply(
	struct probe_packet_st const *packet, int iserr)
{
	ssize_t ret;
	struct iovec iov;
	struct msghdr msg;
	char buf[1024], ctrl[1024];

	/* Set up @msg.  1024 bytes ought to be enough for @buf and @ctrl. */
	memset(&iov, 0, sizeof(iov));
	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof(ctrl);

	if ((ret = recvmsg(packet->rawfd, &msg, iserr ? MSG_ERRQUEUE : 0))
		< 0)
	{
		fprintf(stderr, "%s: recvmsg(SOCK_RAW): %m\n", Prog);
		return NULL;
	} else
		return handle_probe_reply(packet, &msg, buf, ret, iserr);
} /* recv_probe_reply */

static void receive_replies(struct probe_packet_st const *packet,
	nanosecs_t max_timeout)
{
	struct pollfd pfd;

	/* Poll @rawfd for replies. */
	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = packet->rawfd;
	pfd.events = POLLIN | POLLERR;

	/* While there are outstanding @Probes and @max_timeout hasn't
	 * elapsed. */
	while (Probes)
	{
		int n;
		struct probe_st *probe;
		nanosecs_t timeout_ns, elapsed;
		struct timespec timeout, before, now;

		/* @timeout_ns := MIN(oldest probe's timeout, @max_timeout) */
		timeout_ns = Probes[0].timeout;
		if (max_timeout && max_timeout < timeout_ns)
			timeout_ns = max_timeout;
		assert(timeout_ns > 0);
		timeout.tv_sec  = timeout_ns / NANOSECS;
		timeout.tv_nsec = timeout_ns % NANOSECS;

		clock_gettime(CLOCK_MONOTONIC, &before);
		if ((n = ppoll(&pfd, 1, &timeout, NULL)) < 0)
			fprintf(stderr, "%s: ppoll(SOCK_RAW): %m\n", Prog);

		if (n != 0)
		{	/* Calculated the @elapsed time. */
			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed = timespec_diff(&before, &now);
		} else	/* Assume ppoll(2) waited for @timeout_ns. */
			elapsed = timeout_ns;

		if (n > 0 && (probe = recv_probe_reply(packet,
				pfd.revents & POLLERR)) != NULL)
			/* @probe replied, we can forget about it. */
			free_probe(probe);

		expire_probes(elapsed);
		if (!max_timeout)
			/* We are to process all remaining @Probes. */
			continue;
		else if (max_timeout <= elapsed)
			/* We have been waiting enough. */
			break;
		else	/* @max_timeout > @elapsed */
			max_timeout -= elapsed;
	} /* while there are @Probes */

	if (max_timeout > 0)
	{	/* Wait until @max_timeout elapses. */
		struct timespec t;

		t.tv_sec  = max_timeout / NANOSECS;
		t.tv_nsec = max_timeout % NANOSECS;
		nanosleep(&t, NULL);
	}
} /* receive_replies */

/* The main function */
int main(int argc, char *argv[])
{
	unsigned i, tos;
	unsigned nranges;
	int optchar, ret;
	struct timeval now;
	char const *interface;
	struct addrinfo gai, *target;
	struct port_range_st *ranges;
	struct probe_packet_st packet;

	/* Init */
	if ((Prog = strrchr(argv[0], '/')) != NULL)
		Prog++;

	gettimeofday(&now, NULL);
	srand(now.tv_usec);

	memset(&gai, 0, sizeof(gai));
	gai.ai_socktype = SOCK_STREAM;
	gai.ai_protocol = IPPROTO_TCP;

	/* Parse the command line. */
	/* The default @Opt_interval is 1/3s (3*60 probes per minute).
	 * The timeout is a little over 3s to catch locally generated
	 * ICMP errors. */
	tos = 0;
	interface = NULL;
	Opt_verbosity = 1;
	Opt_verify_checksum = -1;
	Opt_interval = NANOSECS / 3;
	Opt_timeout = 3050 * (NANOSECS/1000);
	Opt_report_open = Opt_report_closed = Opt_report_timeout = -1;
	while ((optchar = getopt_long(argc, argv,
			"hvqs46SI:T:kKn:t:aNOCX", Options, NULL)) != -1)
		switch (optchar)
		{
		case 'v':
			/* Print when a probe is sent. */
			Opt_verbosity = 2;
			break;
		case 'q':
			/* Do not print error replies. */
			Opt_verbosity = 0;
			break;
		case 's':
			/* Print Ports_* and Error_* at the end. */
			Opt_statistics = 1;
			break;

		case '4':
			/* Tell getaddrinfo(3) to fetch IPv4 addresses. */
			gai.ai_family = AF_INET;
			break;
		case '6':
			gai.ai_family = AF_INET6;
			break;
		case 'S':
			gai.ai_protocol = IPPROTO_SCTP;
			break;

		case 'I':
			interface = optarg;
			break;
		case 'T':
			/* @tos is expected in hexa. */
			if ((tos = parse_integer(optarg, 1, NULL)) > 0xFF)
				fatal("0x%X: invalid TOS/TCLASS", tos);
			break;

		case 'k':
			Opt_verify_checksum = 1;
			break;
		case 'K':
			Opt_verify_checksum = 0;
			break;

		case 'n':
			/* Time to wait between probes. */
			if (!(i = parse_integer(optarg, 0, NULL)))
				fatal("%u: invalid number of probes per "
					"minute", i);
			Opt_interval = 60 * NANOSECS / i;
			break;
		case 't':
			/* Convert from milliseconds. */
			if (!(i = parse_integer(optarg, 0, NULL)))
				fatal("%u: invalid timeout", i);
			Opt_timeout = NANOSECS / 1000 * i;
			break;

		case 'a':
			Opt_report_open = Opt_report_closed =
				Opt_report_timeout = 1;
			break;
		case 'N':
			Opt_report_open = Opt_report_closed =
				Opt_report_timeout = 0;
			break;
		case 'O':
			Opt_report_open = 1;
			break;
		case 'C':
			Opt_report_closed = 1;
			break;
		case 'X':
			Opt_report_timeout = 1;
			break;

		case 'h':
			printf(Help, Prog);
			exit(0);
		case '?':
			exit(1);
		}

	/* Verify checksum of incoming SCTP packets by default. */
	if (Opt_verify_checksum < 0)
		Opt_verify_checksum = (gai.ai_protocol == IPPROTO_SCTP);

	/* Only report open ports by default. */
	if (Opt_report_open < 0)
		Opt_report_open = Opt_report_closed < 0
					&& Opt_report_timeout < 0;
	if (Opt_report_closed < 0)
		Opt_report_closed = 0;
	if (Opt_report_timeout < 0)
		Opt_report_timeout = 0;

	/* Get the @target address. */
	argv += optind;
	if (!*argv)
		fatal("which host to scan?");

	target = NULL;
	if ((ret = getaddrinfo(*argv, NULL, &gai, &target)) != 0)
		fatal("%s: %s", *argv, gai_strerror(ret));
	else if (!target)
		fatal("getaddrinfo(%s): NULL", *argv);
	else if (target->ai_family != AF_INET && target->ai_family != AF_INET6)
		fatal("%s: unsupported address family", *argv);

	memset(&packet, 0, sizeof(packet));
	packet.af = target->ai_addr->sa_family;
	packet.proto = gai.ai_protocol;
	memcpy(&packet.dst_addr, target->ai_addr, target->ai_addrlen);

	/* Parse the port ranges to scan. */
	if (argc <= optind + 1)
		fatal("which ports to scan?");
	nranges = argc - optind - 1;
	if (!(ranges = malloc(sizeof(*ranges) * nranges)))
		fatal("malloc(): %m");
	for (i = 0; i < nranges; i++)
		parse_port_range(&ranges[i], argv[1 + i]);

	/* Prepare */
	setvbuf(stdout, NULL, _IOLBF, 0);
	init_probe_socket(&packet, interface, tos);
	init_probe_packet(&packet);

	/* In case of multiple addresses, print the chosen one.
	 * Defer this message until everything is ready to go. */
	if (target->ai_next || Opt_verbosity > 1)
	{
		char str[INET6_ADDRSTRLEN];

		if (getnameinfo(target->ai_addr, target->ai_addrlen,
				str, sizeof(str), NULL, 0,
				NI_NUMERICHOST) == 0)
			printf("Scanning %s ...\n", str);
		else	/* Points to the hostname. */
			printf("Scanning %s ...\n", *argv);
	}
	freeaddrinfo(target);

	/* Run */
	for (i = 0; i < nranges; i++)
	{
		unsigned port;

		/* For each @port send a probe then wait for replies
		 * for @Opt_interval. */
		for (port = ranges[i].port_from; port <= ranges[i].port_until;
			 port++)
		{
			struct probe_st *probe;

			if (!(probe = send_probe_packet(&packet, port)))
				continue;

			if (Last_probe)
			{	/* Append @probe to @Probes. */
				probe->prev = Last_probe;
				Last_probe->next = probe;
				Last_probe = probe;
			} else	/* This is the only outstanding @probe. */
				Probes = Last_probe = probe;

			receive_replies(&packet,
				/* Is this the last @port to scan? */
				port < ranges[i].port_until || i+1 < nranges
				? Opt_interval : 0);
		} /* for each port */
	} /* for each port range */

	/* We must have consumed all outstanding @Probes. */
	assert(!Probes);
	free(ranges);

	if (Opt_statistics)
	{
		printf("Ports open: %u, closed: %u, timed out: %u\n",
			Ports_open, Ports_closed, Ports_timed_out);
		if (Error_sending || Error_responses || Unexpected_responses)
			printf("Error sending: %u, error responses: %u, "
				"unexpected responses: %u\n",
				Error_sending, Error_responses,
				Unexpected_responses);
	}

	return 0;
} /* main */

/* End of mininmap.c */
