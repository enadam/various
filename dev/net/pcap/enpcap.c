/*
 * enpcap.c -- encapsulate application-layer data in PCAP files
 *
 * This program enables you to analyse application protocol messages with
 * Wireshark and friends.  This is useful if capturing live traffic is not
 * practical but you have samples of received/sent bytes from server logs.
 * 
 * Another use case could be crafting (possibly intentionally malformed)
 * application protocol messages by hand for testing purposes.  This case
 * you can use Wireshark through enpcap to verify that the contents of the
 * packets are as desired.
 *
 * Synopsis:
 *   enpcap -oO <output-fname> [-sd <port>] [-hHxb] [<input>]...
 *
 * Options:
 *   -o <output-fname>		Tells the output file name.  If none is
 *				specified the standard output is used.
 *				Note that the file must be seekable.
 *   -O <output-fname>		Likewise, but the output is not in PCAP
 *				format, but a hexadecimal string.  This is
 *				mainly useful for debugging the progrram,
 *				but combined with the binary input format
 *				it can be effectively used as a bin->hex
 *				converter.
 *   -s <port>, -d <port>	The port numbers for the transport protocol.
 *   -h				Selects packet per paragraph hexa format for
 *				the subsequent input files. (defaul)
 *   -H				Selects packet per line hexadecimal input.
 *   -x				Selects xxd-like input format.
 *   -b				Selects binary input.
 *
 * Application data in the input files are broken into packets, which are
 * framed individually.  Each packet is in a separate IPv4 packet, in an
 * SCTP DATA chunk.  SCTP was chosen as transport protocol wire format
 * because it appeared to be simpler than TCP's.  There's no L2 (Ethernet)
 * framing in the output PCAP.  It should be noted that PCAP doesn't allow
 * arbitrarily large payload.  The maximum size is somewhat lower than 64KiB.
 *
 * The port numbers adjustable with -sd are the ones in the SCTP headers.
 * If your application protocol is HTTP for example it is worth setting
 * one of the ports to 80, so Wireshark will know it's HTTP.  The default
 * ports are 2222 and 3868 (Diameter).
 *
 * The binary (-b) input format simply reads the input and writes to the
 * output PCAP with the headers.  Each input file is a separate packet.
 * Empty files result in empty packets (ie. all the headers present but
 * no application data).
 *
 * The xxd (-x) input format is meant to make it easier to convert between
 * PCAP and hexa.  The format is like:
 * 0000000: 7f45 4c46 0201 0100 0000 0000 0000 0000  .ELF............
 * The hexadecimal digits may be grouped in any way.  Everything else is
 * ignored.  Empty lines separate packets.  Empty paragraphs (sequence of
 * empty lines) result in empty packets.  Lines can be commented out with
 * a leading '#'.  Lines consisting of only whitespace are considered empty.
 *
 * Packet per paragraph (-h) input format is similar except that no offset
 * is expected, but all characters are significant.  Packets are separated
 * by empty lines.  Comments are allowed anywhere, and any non-alphanumeric
 * character beside whitespace can be used to delimit the hexa bytes.  Empty
 * packets are generated for lines starting with the "EMPTY" word.
 *
 * In the packet per line format each line designates a separate packet.
 * Empty lines result in empty packets.  Comments are allowed anywhere.
 *
 * Development ideas:
 * -- understand tcpdump -xx
 * -- better documentation of hexa input formats
 * -- filter mode
 * -- support jumbograms
 */

/* Include files */
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/time.h>
#include <netinet/ip.h>

/* Standard definitions */
#define PCAP_MAGIC                  	0xA1B2C3D4
#define PCAP_VERSION_MAJOR          	2
#define PCAP_VERSION_MINOR          	4
#define PCAP_DLT_RAW_IPV4           	228
#define DFLT_SRC_PORT			2222
#define DFLT_DST_PORT			3868

/* Type definitions */
/* PCAP header */
struct pcap_hdr_st
{
	uint32_t magic;			/* PCAP_MAGIC		*/
	uint16_t major, minor;		/* PCAP_VERSION_*	*/
	int32_t  unused1, unused2;	/* obsolete		*/
	uint32_t snaplen;		/* frame max size	*/
	uint32_t data_link;		/* PCAP_DLT_RAW_IPV4	*/
};

/* PCAP per packet header */
struct pcap_pkt_hdr_st
{
	uint32_t recv_sec, recv_usec;	/* time of receipt	*/
	uint32_t pkt_size, orig_size;	/* size of the capture	*/
					/* and original size	*/
} __attribute__((packed));

/* Common SCTP header */
struct sctp_common_header_st
{
	uint16_t src_port, dst_port;
	uint32_t verification_tag, checksum;
} __attribute__((packed));

/* Header of an SCTP DATA chunk */
struct sctp_data_header_st
{
	uint8_t chunk_type;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t final_fragment:	1;
	uint8_t first_fragment:	1;
	uint8_t chunk_unordered:1;
	uint8_t reserved_flags:	5;
#else
# warning "Not tested on non-little-endian machines."
	uint8_t reserved_flags:	5;
	uint8_t chunk_unordered:1;
	uint8_t first_fragment:	1;
	uint8_t final_fragment:	1;
#endif
	uint16_t chunk_length;

	uint32_t transmission_sequence_number;

	uint16_t stream_identifier;
	uint16_t stream_sequence;

	uint32_t payload_protocol_identifier;
} __attribute__((packed));

/* All PCAP and network headers together.  We directly encapsulate IPv4
 * without data link layer protocol (eg. Ethernet).  The transmission
 * protocol is SCTP, which seemed to be simpler than TCP. */
struct net_hdr_st
{
	struct pcap_pkt_hdr_st pcap;
	struct iphdr ip;
	struct
	{
		struct sctp_common_header_st common;
		struct sctp_data_header_st data;
	} __attribute__((packed)) sctp;
} __attribute__((packed));

/* Private variables */
/* The source and destination port to use in the SCTP layer. */
static unsigned Opt_sport = DFLT_SRC_PORT, Opt_dport = DFLT_DST_PORT;

/* The current time to be written to pcap_pkt_hdr_st::recv_*.
 * Since we're operating in bulk, there's no sense in querying
 * the time over and over. */
static struct timeval Now;

/* Program code */
/* Begin a PCAP file.  Since we don't know @snaplen beforehand, writing
 * this header is actually the last step in creating the PCAP file. */
static void write_pcap_file_header(FILE *st, size_t snaplen)
{
	struct pcap_hdr_st pcap;

	fseek(st, 0, SEEK_SET);
	memset(&pcap, 0, sizeof(pcap));
	pcap.magic	= PCAP_MAGIC;
	pcap.major	= PCAP_VERSION_MAJOR;
	pcap.minor	= PCAP_VERSION_MINOR;
	pcap.snaplen	= snaplen;
	pcap.data_link	= PCAP_DLT_RAW_IPV4;
	fwrite(&pcap, sizeof(pcap), 1, st);
} /* write_pcap_file_header */

/*
 * Write a PCAP packet header, IP header and an SCTP DATA chunk whose payload
 * size @spayload to @st at *@posp.  Initially *@posp should be zero, then it
 * is maintained by this function as a state variable to know where to write
 * the next packet header next time.
 *
 * If the packet has payload the file's position is not changed.  Otherwise
 * an empty packet is written and @st's position is advanced by the number
 * of bytes in the headers.
 */
static void write_pcap_packet_header(char const *fname, FILE *st,
	off_t *posp, size_t spayload)
{
	ssize_t err;
	unsigned checksum;
	struct net_hdr_st pkt;

	/* -O output? */
	if (!posp)
	{	/* Newline ends the packet. */
		putc('\n', st);
		return;
	}

	/* Is the application data too much? */
	if (spayload > 65535 - (sizeof(pkt.ip) + sizeof(pkt.sctp)))
		fprintf(stderr, "%s: packet too large for IP (%zu bytes)\n",
			fname, spayload);

	memset(&pkt, 0, sizeof(pkt));
	pkt.pcap.recv_sec  = Now.tv_sec;
	pkt.pcap.recv_usec = Now.tv_usec;
	pkt.pcap.pkt_size  = sizeof(pkt.ip) + sizeof(pkt.sctp) + spayload;
	pkt.pcap.orig_size = pkt.pcap.pkt_size;

	pkt.ip.version	= 4;
	pkt.ip.ihl	= sizeof(pkt.ip) / sizeof(uint32_t);
	pkt.ip.tot_len	= htons(pkt.pcap.pkt_size);
	pkt.ip.ttl	= 16;
	pkt.ip.protocol	= IPPROTO_SCTP;
	pkt.ip.saddr	= htonl(INADDR_LOOPBACK);
	pkt.ip.daddr	= htonl(INADDR_LOOPBACK);

	/* Calculate the IP checksum. */
#if __BYTE_ORDER != __LITTLE_ENDIAN
# warning "This code has only been tested on little-endian machines,"
# warning "and is likely to break on big-endian."
#endif
	checksum = ntohs((pkt.ip.version << 12)
			+ (pkt.ip.ihl << 8)
			+ (pkt.ip.tos << 0))
		+ pkt.ip.tot_len
		+ pkt.ip.id
		+ pkt.ip.frag_off
		+ ntohs((pkt.ip.ttl << 8) + pkt.ip.protocol)
		+ pkt.ip.check
		+ ((pkt.ip.saddr >> 16) & 0xFFFF) + (pkt.ip.saddr & 0xFFFF)
		+ ((pkt.ip.daddr >> 16) & 0xFFFF) + (pkt.ip.daddr & 0xFFFF);
	checksum += (uint16_t)(checksum >> 16);
	pkt.ip.check = ~(uint16_t)checksum;

	/* Do not caluculate SCTP checksum; it's not verified by Wireshark. */
	pkt.sctp.common.src_port = htons(Opt_sport);
	pkt.sctp.common.dst_port = htons(Opt_dport);

	pkt.sctp.data.first_fragment = 1;
	pkt.sctp.data.final_fragment = 1;
	pkt.sctp.data.chunk_length = htons(sizeof(pkt.sctp.data) + spayload);

	/* Write out $pkt. */
	if (spayload > 0)
	{	/* We need to flush $st to empty its write buffer.
		 * fseek() would have done the same. */
		fflush(st);
		err = pwrite(fileno(st), &pkt, sizeof(pkt), *posp) < 0;
	} else
		err = fwrite(&pkt, sizeof(pkt), 1, st) != 1;
	if (err)
	{	/* Failed */
		fprintf(stderr, "%s: %m\n", fname);
		exit(1);
	} else	/* Remember where to write the next packet header. */
		*posp += sizeof(pkt) + spayload;
} /* write_pcap_packet_header */

/* A byte has been parsed and now is output to $st. */
static void output_byte(FILE *st, int c, off_t *posp, size_t payload)
{
	if (!posp)
	{	/* -O output format */
		fprintf(st, "%.2x", c);
		return;
	} else if (!payload)
		/* First payload byte, leave room for all the headers. */
		fseek(st, sizeof(struct net_hdr_st), SEEK_CUR);
	putc(c, st);
} /* output_byte */

/* Unexpected non-hexadecimal digit. */
static void __attribute__((noreturn))
unhex_error(char const *fname, unsigned lineno, char c)
{
	fprintf(stderr, "%s:%u: %c: invalid hex character\n",
		fname, lineno, c);
	exit(1);
} /* unhex_error */

/* Return the numeric value of a hexa character. */
static unsigned __attribute__((pure))
unhex(char const *fname, unsigned lineno, char c)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	else if ('A' <= c && c <= 'F')
		return 10 + (c-'A');
	else if ('a' <= c && c <= 'f')
		return 10 + (c-'a');
	else
		unhex_error(fname, lineno, c);
} /* unhex */

/* Implement the -hH input formats. */
static size_t hex(char const *input, FILE *sin, int para,
	char const *output, FILE *sex, off_t *posp)
{
	size_t n, max;
	unsigned lineno;
	unsigned char byte;
	int is_nibble, all_whitespace, empty_packet;

	byte = 0;
	lineno = 1;
	max = n = 0;
	is_nibble = 0;
	all_whitespace = 1;
	empty_packet = 0;
	for (;;)
	{
		int c;

		/* Read the next character or EOF. */
		c = getc(sin);

		/*
		 * If we already have a nibble in $byte and either we get
		 * its lower half or $byte is terminated by an EOF, comment,
		 * whitespace or a delimiter, we need to flush it.  This is
		 * the only place we output payload.
		 */
		if (is_nibble)
		{
			/* If we have a full byte to output (==> and possibly
			 * a start a new one) read the next nibble, so that
			 * the if() below will have something to do. */
			if (isalnum(c))
			{
				byte = (byte << 4) | unhex(input, lineno, c);
				c = getc(sin);
			}

			/* Output $byte. */
			assert(!all_whitespace);
			output_byte(sex, byte, posp, n++);
			is_nibble = 0;
			byte = 0;
		} /* $byte was a nibble */

		/* Generate an $empty_packet? */
		assert(!is_nibble);
		if (all_whitespace && c == 'E')
		{	/* Check whether subsequent input reads "EMPTY". */
			if ((c = getc(sin)) != 'M')
			{	/* Continue with processing $c == 'E'. */
				ungetc(c, sin);
				c = 'E';
			} else if (getc(sin) != 'P')
				/* Invalid hex sequence started with 'M'. */
				unhex_error(input, lineno, 'M');
			else if (getc(sin) != 'T')
				unhex_error(input, lineno, 'M');
			else if (getc(sin) != 'Y')
				unhex_error(input, lineno, 'M');
			else if (!isspace(c = getc(sin))
				 	&& c != '#' && c != EOF)
				unhex_error(input, lineno, 'M');
			else
			{	/* "EMPTY" is followed by a whitespace
				 * (including '\n'), or a '#' or an EOF.
				 * Emit an $empty packet. */
				empty_packet = 1;
				if (c != EOF)
					c = '\n';
				/* $c is either '\n' or EOF. */
			}
		} /* "EMPTY" */

		assert(!is_nibble);
		if (isalnum(c))
		{	/* Expect a hexadecimal digit. */
			assert(byte == 0);
			byte = unhex(input, lineno, c);
			is_nibble = 1;

			/* This line contains at least one hexa digit. */
			all_whitespace = 0;
		} else if (c == '#' || c == '\n' || c == EOF)
		{	/* A comment, newline or end of file reached. */
			/*
			 * End of packet?
			 * -- in case of EOF, certainly it is
			 * -- if we're not in @para mode a newline or a
			 *    comment ends the packet.
			 * -- otherwise if we're in @para mode, the packet is
			 *    ended if the line only consisted of whitespace
			 */
			if (n > 0
				&& (c == EOF
					|| !para /* && ('\n' || '#') */
					|| (c == '\n' && all_whitespace)))
			{
				write_pcap_packet_header(output,sex,posp,n);
				if (max < n)
					max = n;
				n = 0;
			}

			/* Write an empty packet? */
			if (empty_packet)
			{
				empty_packet = 0;
				write_pcap_packet_header(output,sex,posp,0);

				/* Skip to the end of the line. */
				c = '#';
			}

			if (c == '#')
			{	/* Skip to the end of line (comment). */
				do
					c = getc(sin);
				while (c != '\n' && c != EOF);
			}

			/* Out of input? */
			if (c == EOF)
				break;
			lineno++;

			/* Initially a new line is supposed to be empty. */
			all_whitespace = 1;
		} else if (!isspace(c))
			/* Delimiter */
			all_whitespace = 0;
	} /* until EOF */

	/* Return the largest payload size we encountered. */
	return max;
} /* hex */

/* Implement the -x input format. */
static size_t xxd(char const *input, FILE *sin,
	char const *output, FILE *sex, off_t *posp)
{
	size_t n, max;
	unsigned lineno;

	/*
	 * Process the file line by line:
	 *
	 * 0000000: 7f45 4c46 0201 0100 0000 0000 0000 0000  .ELF............
	 *
	 * Everything other than the hexa characters are ignored.
	 */
	lineno = 1;
	max = n = 0;
	for (;;)
	{
		int c;
		unsigned i;

		/* Skip empty lines. */
newline:	do
		{
			c = getc(sin);
			if (c == EOF)
				goto eof;
			if (c == '#')
				goto skip_to_newline;
			if (c == '\n')
			{	/* End of packet. */
				write_pcap_packet_header(output,sex,posp,n);
				if (max < n)
					max = n;
				n = 0;

				lineno++;
				goto newline;
			}
		} while (isspace(c));
		ungetc(c, sin);

		/* Eat the prefixing offset ("0000000:"). */
		fscanf(sin, "%*x:%n", &i);
		if (!i)
		{	/* We can't continue, because we don't know how many
			 * characters fscanf() has consumed. */
			fprintf(stderr, "%s:%u: syntax error\n",
				input, lineno);
			exit(1);
		}

		/* Process the hexa characters. */
		for (;;)
		{
			c = getc(sin);
			if (c == EOF)
				goto eof;
			if (c == '\n')
			{	/* Does not terminate the packet. */
				lineno++;
				goto newline;
			} else if (c == ' ')
			{	/* End of hexa string? */
				c = getc(sin);
				if (c == EOF)
					goto eof;
				if (c == ' ')
					/* Double space. */
					goto skip_to_newline;
			}
			ungetc(c, sin);

			/* Parse a one or two-character hex number. */
			if (fscanf(sin, "%2x", &i) != 1)
			{
				fprintf(stderr, "%s:%u: syntax error\n",
					input, lineno);
				exit(1);
			}

			output_byte(sex, i, posp, n++);
		} /* for each (pair) of hexa characters */

skip_to_newline: /* Skip the textual representation of the hexa string. */
		do
		{
			c = getc(sin);
			if (c == EOF)
				goto eof;
		} while (c != '\n');
		lineno++;
	} /* until end of file */

eof:	if (n > 0)
		/* Flush ongoing packet. */
		write_pcap_packet_header(output, sex, posp, n);
	return max > n ? max : n;
} /* xxd */

/* Implement the -b input format. */
static size_t binary(FILE *sin, char const *output, FILE *sex, off_t *posp)
{
	int c;
	size_t n;

	for (n = 0; (c = getc(sin)) != EOF; n++)
		output_byte(sex, c, posp, n);

	/* Finish the packet (be it empty or not). */
	write_pcap_packet_header(output, sex, posp, n);

	return 1;
} /* binary */

/* The main function */
int main(int argc, char *argv[])
{
	off_t pos;
	char format;
	size_t maxlen;
	FILE *sin, *sex;
	int optchar, ohex;
	char const *input, *output;

	/* Help? */
	if (argc == 2 && !strcmp(argv[1], "--help"))
	{
		puts("pcap [-oO <output-fname>] [-sd <port>] "
			"[[-hHxb] <input>]...");
		return 0;
	}

	/* Parse the command line. */
	ohex = 0;
	output = NULL;
	format = 'h';
	while ((optchar = getopt(argc, argv, "o:O:s:d:hHxb")) != EOF)
		switch (optchar)
		{
		case 'o':
			output = optarg;
			break;
		case 'O':
			ohex = 1;
			output = optarg;
			break;
		case 's':
			/* Verify the port range. */
			Opt_sport = atoi(optarg);
			if (!Opt_sport || Opt_sport > 65535)
			{
				fprintf(stderr,
					"enpcap -s %u: invalid port\n",
					Opt_sport);
				return 1;
			}
			break;
		case 'd':
			Opt_dport = atoi(optarg);
			if (!Opt_dport || Opt_dport > 65535)
			{
				fprintf(stderr,
					"enpcap -d %u: invalid port\n",
					Opt_dport);
				return 1;
			}
			break;
		case 'b': /* Input is in binary. */
		case 'H': /* Input is in hexa lines. */
		case 'h': /* Input is in hexa paragraphs. */
		case 'x': /* Input is like xxd's output. */
			/* Format of the input */
			format = optchar;
			break;
		default:
			return 1;
		} /* while there're options */

	/* Open the output file. */
	if (!output || !strcmp(output, "-"))
	{	/* It's OK if @stdout is redirected to a file. */
		sex = stdout;
		output = "(stdout)";
	} else if (!(sex = fopen(output, "w")))
	{
		fprintf(stderr, "%s: %m\n", output);
		return 1;
	}

	/* We need a seekable @output because sizes are included
	 * in the headers. */
	pos = sizeof(struct pcap_hdr_st);
	if (!ohex && fseek(sex, pos, SEEK_SET) < 0)
	{	/* Output is not seekable. */
		if (errno == ESPIPE)
			fprintf(stderr, "%s: output needs to be seekable\n",
				output);
		else
			fprintf(stderr, "%s: %m\n", output);
		return 1;
	}

	/* @Now is used by write_pcap_packet_header() to write timestamps. */
	gettimeofday(&Now, NULL);
	maxlen = 0;
	do
	{
		size_t n;

		/* Get the input file. */
		input = argv[optind];
		if (input)
			optind++;
		if (!input || !strcmp(input, "-"))
		{
			sin = stdin;
			input = "(stdin)";
		} else if (!(sin = fopen(input, "r")))
		{
			fprintf(stderr, "%s: %m\n", input);
			return 1;
		}

		/* Convert */
		if (format == 'x')
			n = xxd(input, sin, output, sex, ohex ? NULL : &pos);
		else if (format != 'b')
			n = hex(input, sin, format == 'h', output, sex,
				ohex ? NULL : &pos);
		else	/* $format == 'b' */
			n = binary(sin, output, sex, ohex ? NULL : &pos);

		/* Update $maxlen (if relevant) and close $sin. */
		if (maxlen < n)
			maxlen = n;
		if (sin != stdin)
			fclose(sin);
	} while (argv[optind]);

	/* Now that we know $maxlen, finish the PCAP file. */
	if (!ohex)
		write_pcap_file_header(sex, maxlen);
	fclose(sex);

	return 0;
} /* main */

/* End of enpcap.c */
