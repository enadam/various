/*
 * radiator.cc -- a dummy DIAMETER server and client {{{
 *
 * This simple program is able to perform Capability-Exchange, sends
 * Device-Watchdog periodically, and understands Device-Peer-Request.
 * Moreover you can send User-Data-Requests and Push-Notifications.
 * Finally, radiator can answer as well all of these requests.
 *
 * Synopses:
 *   radiator --help
 *   radiator [<options>]... [<parameters>]...
 *
 * Options:
 * -v, --verbose            Increase verbosity level.  At the default level,
 *                          command codes of sent and received messages are
 *                          printed.  One level above the sent and received
 *                          number of bytes are printed as well.  On the next
 *                          level and above the sent and received Diameter
 *                          messages are decoded and shown.
 * -q, --quiet              Decrease verbosity level.  At zero level, nothing
 *                          traffic-related is printed.
 *
 * -c, --client             These options select what requests to send,
 * -s, --server             what Origin/Destination-Host/Realm to use etc.
 * -L, --lbsdia             Indicates that the peer is DiaLBS and changes
 *                          the default Destination-{Host,Realm} accordingly.
 *
 * -S, --no-stdin           Do not read stdin for commands.  This way the
 *                          program can be put to the background.
 * -D, --no-reply           Do not reply to URD:s and PNR:s.
 * -N, --no-net             Do not communicate at all.  This is only useful
 *                          to capture the tool's would-be output with the
 *                          -o option.  Wathdogging is disabled implicitly.
 *                          -S and -N are not compatible.
 *
 * -O, --write-input <fname>  Write everything sent or received to <fname>
 *                            in PCAP format with fake IP and SCTP header.
 * -o, --write-output <fname> The output file is truncated and overwritten.
 * -w, --write <fname>        Write both input and output to <fname>.  "-"
 *                            designates the standard output.
 *
 * Parameters:
 * -i, --hop-by-hop         Specify low 16 bits of the Hop-by-Hop Id with
 *                          which all messages except PNR are sent.
 *                          The default is 3333.  If only the lower 16 bits
 *                          are specified the high bits will be filled with
 *                          the local port number of the connection.
 * -I, --end-to-end         The End-to-End Id, defaulting to 4444.
 * -h, --origin-host <str>  Sets the Origin-Host.  The default is
 *                          "radiator-{client,server}-host".
 * -r, --origin-realm <str> Sets the Origin-Realm.  The default is
 *                          "radiator-{client,server}-realm".
 * -H, --dest-host <str>    Sets the Destination-Host.  If we're talking
 *                          to DiaLBS (-L was specified), the default is
 *                          "lbsdia-host".  Otherwise it's "radiator-*-host".
 * -R, --dest-realm <str>   Sets the Destination-Realm.  If -L is active,
 *                          the default is "lbsdia-realm".  Otherwise it is
 *                          "radiator-*-realm".
 *
 * -t, --watchdog <time>    How much to wait in seconds between two DWRs.
 *                          You can specify a fractional number as well.
 *                          Specify 0 to disable watchdogging permanently
 *                          (so it can't be enabled with the "watchdog"
 *                           command in run-time).
 * -u, --send-delay <time>  Time to wait in (sub)milliseconds before sending
 *                          the next UDR/PNR while conducting a measurement.
 *                          Effectively with this option you can control
 *                          the rate of the requests per second.
 * -U, --recv-delay <time>  Delay responding to UDR/PNR by this number of
 *                          (sub)milliseconds.  This is useful to simulate
 *                          non-zero processing time of requests, or with
 *                          the -D (--no-reply) option induce congestion
 *                          on the network connection.
 *
 * -a, --min-stream <lo>, -A, --max-stream <hi>
 *                          When sending an UDR, choose the stream number
 *                          from [<lo>, <hi>].  With this parameter you can
 *                          control on which external connection will DiaLBS
 *                          forward the message.
 * -b, --min-hbh <lo>, -B, --max-hbh <hi>
 *                          When sending a PNR, choose the high 16-bits
 *                          of the Hop-by-Hop Id of the message from
 *                          [<lo>, <hi>].  With this parameter you can
 *                          control on which internal connection will
 *                          DiaLBS forward the message.
 * -m, --min-payload <min>, -M, --max-payload <max>
 *                          Specifies the minimum and maximum size of
 *                          User-Data in UDA and PNR.
 *
 * radiator itself doesn't make network connections.  It expects its
 * standard output to be an already connected socket.  This socket can
 * have any protocols as long as it accepts read() and write().  However,
 * if you intend to use multiple streams, the socket must be IPPROTO_SCTP.
 *
 * Compilation of the program:
 *   c++ -Wall -O2 -pthread -lrt -lsctp radiator.cc -o radiator
 *
 * Usage: with sicktp
 *   sicktp -p1 -s 4444 127.0.0.1 -x radiator       # server side
 *   sicktp -p1 -d 4444 127.0.0.1 -x radiator -c    # client side
 *
 * While the program is running you can issue commands on the standard input:
 * # <anything>                         Skip the line.
 * verbosity [<level>]                  Show or change the verbosity level.
 * verbose | quiet                      Change the verbosity level by 1 (-vq).
 * role                                 Tell whether we're server or client.
 * <newline>                            Send a UDR (in client mode) or a
 *                                      PNR (in server mode) to the peer.
 *                                      You cannot send anything while a
 *                                      measurement is in progress.
 * [!] <number-of-messages>             Send <number-of-messages>.  Without
 *                                      the '!' prefix measure how much time
 *                                      it takes to receive answers to them.
 * [!] [<n>] rnd                        Send one or more DIAMETER messages
 *                                      with randomized AVPs and possibly
 *                                      start a measurement.
 * [!] [<n>] hexa [-hH] [<hexa-string>] Just like <newline>, except that
 *                                      the message contents are taken from
 *                                      <hexa-string>, a string of hexadecimal
 *                                      digits separated by spaces or '_'.
 *                                      By default a DIAMETER header is added
 *                                      to it.  The -h option replaces the
 *                                      existing header of <hexa-string> with
 *                                      ours own.  On the other hand -H leaves
 *                                      <hexa-string> as it is (which has to
 *                                      have a DIAMETER header on its own).
 * [!] [<n>] file [-bHh] <fname>        Likewise, except that the contents are
 *                                      loaded from <fname>.  -b designates
 *                                      binary file, not hexadecimal.
 * ?                                    Print the Session-Id counters.
 *                                      Only useful for debugging.
 * cancel                               Cancel the ongoing measurement.
 * noreply | doreply                    Do or do not (-D) reply to UDR/PNR.
 * watchdog     [<period>]              Display or change the time between
 *                                      periodic DWR probes.  0 disables it
 *                                      temporarily (-t).
 * send-delay   [<time>]                Show or change the time to wait
 *                                      between subsequent UDR/PNR:s (-u).
 * recv-delay   [<time>]                Show or change the time to wait
 *                                      before replying to a UDR/PNR (-U).
 * streams      [{<exact>|<min> <max>}] Dispatch UDRs on these streams (-aA)
 *                                      or show the limits.
 * lga          [{<exact>|<min> <max>}] Dispatch PNRs with these numbers in
 *                                      the high 16 bit of the HbH Id (-bB).
 * user-data    [{<exact>|<min> <max>}] Add this much data to User-Data (-mM)
 *                                      or show the limits.
 * ^D, ^C                               End the program.
 *
 * When radiator starts normally it creates two threads: one to process
 * user commands and another for watchdogging.  The main thread handles
 * incoming network traffic until it's interrupted with SIGINT or SIGTERM.
 *
 * Many structs and classes have been stolen from LBSDIACore and effort is
 * made to keep them synchronized.
 * }}}
 */

// TODO: date command
// TODO: display time when measurement starts/ends
// TODO: noenter

/* Include files {{{ */
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <setjmp.h>
#include <ctype.h>
#include <limits.h>

#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/sctp.h>

#include <vector>
// }}}

/* Type definitions and macros {{{ */
/* DX platform definitions {{{ */
/* Address format in ip_addr_t.version. */
#define IP_ADDR_T_IPV4_C            0x0
#define IP_ADDR_T_IPV6_C            0x1
#define IP_ADDR_T_NOT_DEF_C         0xFF

typedef uint8_t byte;
typedef uint16_t word;
typedef char character_t;
typedef struct _Sip_addr_t {
   byte  version ;

    /* #E: IP version. Either IPv4 or IPv6
      #CDRID:0001 */
   byte  addr [16] ;

    /* #E: IP address.
      #CDRID:0002 */

}  ip_addr_t ;
// }}}

/* LBSDiaCore definitions {{{ */
#define LBSDIA_VENDOR_ID            Diameter::VENDOR_NSN
#define LBSDIA_PRODUCT_NAME         "Open_TAS"
#define LBSDIA_FIRMWARE_REVISION    1
#define LBSDIA_SUPPORTED_VENDOR_ID  Diameter::VENDOR_3GPP

#define DIAASSERT                   assert
#define ERR(str, args...)           fprintf(stderr, \
                                            "error: " str "\n", ##args)
#define LOG(str, args...)           fprintf(stderr, str "\n", ##args)
#define DBG(str, args...)           fprintf(stderr, \
                                            "debug: " str "\n", ##args)
#define NOP()                       { /* NOP */ }

// PAD4(n) returns the number of bytes needed for @n to be dividable by 4.
// ALIGN4(n) rounds @n up to the next number dividable by 4.
#define PAD4(n)                     ((4 - (n)) & 3)
#define ALIGN4(n)                   ((n) + PAD4(n))

#define MEMBS_OF(ary)               (sizeof(ary) / sizeof((ary)[0]))
#define CAST(type, var)             reinterpret_cast<type>(var)
#define FILL_STRUCT(st)             /* NOP */

// Returns the number of digits of the maximal value of T, which can be
// an unsigned type, a variable or a literal integer (possibly with a
// size-specifying suffix).  This is useful to pre-determine the maximum
// buffer size for the string representation of an integer.  #include
// <limits.h> to use this macro (for CHAR_BIT).
//
// The fraction 643/2136 approximates log10(2) to 7 significant digits.
// (This code is stolen from libstdc++, std::numeric_limits<T>::digits10
//  doesn't work as expected.)
#define MAXDIGITS_OF(T)             (sizeof(T)*CHAR_BIT * 643L/2136 + 1)
// }}}

/* Our definitions {{{ */
#define PCAP_MAGIC                  0xA1B2C3D4
#define PCAP_VERSION_MAJOR          2
#define PCAP_VERSION_MINOR          4
#define PCAP_DLT_RAW_IPV4           228
#define PCAP_MAX_SNAPLEN            65535
#define SCTP_PPID_DIAMETER          46
#define DIAMETER_SERVER_PORT        3868
#define DIAMETER_CLIENT_PORT        2222
#define DIAMETER_PORTS(ctx)         \
    ((ctx)->is_client ? DIAMETER_SERVER_PORT : DIAMETER_CLIENT_PORT), \
    ((ctx)->is_client ? DIAMETER_CLIENT_PORT : DIAMETER_SERVER_PORT)

// This is the TYPE_0 LCG from glibc 2.19 and has been brought there
// because we need a lot of random numbers and performance matters.
#define srand(seed)                 (MyRanda = (seed))
#define rand() \
	(MyRanda = ((MyRanda * 1103515245) + 12345) & 0x7fffffff)

/* PCAP header */
typedef struct {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;	    /* GMT->local timezone correction in secs (0).  */
    uint32_t sigfigs;	    /* Accuracy of timestamps (0 in practice).      */
    uint32_t snaplen;	    /* Max size of packet captures (65535 usually). */
    uint32_t network;	    /* Data link type (raw IPv4 in our case).       */
} __attribute__((packed)) pcap_hdr_t;

/* PCAP per packet header */
typedef struct {
    uint32_t ts_sec, ts_usec;       /* Time of packet receipt/sendout. */
    uint32_t incl_len, orig_len;    /* # of octets included in the capture,
                                     * and the actual size of the packet. */
} __attribute__((packed)) pcap_pkt_hdr_t;

/* Common SCTP header */
typedef struct {
    uint16_t src_port, dst_port;
    uint32_t verification_tag, checksum;
} __attribute__((packed)) sctp_common_header_t;

/* Header of a DATA chunk */
typedef struct {
    uint8_t chunk_type;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t final_fragment:	 	1;
    uint8_t first_fragment:	 	1;
    uint8_t chunk_unordered: 	1;
    uint8_t reserved_flags:	 	5;
#else
# warning "Not tested on non-little-endian machines."
    uint8_t reserved_flags:	 	5;
    uint8_t chunk_unordered: 	1;
    uint8_t first_fragment:	 	1;
    uint8_t final_fragment:	 	1;
#endif
    uint16_t chunk_length;

    uint32_t transmission_sequence_number;

    uint16_t stream_identifier;
    uint16_t stream_sequence;

    uint32_t payload_protocol_identifier;
} __attribute__((packed)) sctp_data_header_t;

/* All network headers together */
typedef struct {
    pcap_pkt_hdr_t              pcap;
    struct iphdr                ip;
    struct {
        sctp_common_header_t    common;
        sctp_data_header_t      data;
    } __attribute__((packed)) sctp;
} __attribute__((packed)) net_hdr_t;

/* All fields except @sfd are configurable by command line arguments. */
struct ConnectionCtx {
    int sfd;
    bool is_eof;
    bool is_sctp;

    // -iI
    unsigned hop_by_hop, end_to_end;

    // -hrHR
    struct
    {
        char const *host, *realm;
    } origin, destination;

    // -cs, -D
    bool is_client, no_reply;

    // -t: watchdog period in microseconds
    unsigned watchdog_timeout;

    // -aA: the streams to send UDRs on
    unsigned min_stream, max_stream;

    // -bB: the high 16 bits of Hop-by-Hops to send PNRs with
    unsigned min_lga, max_lga;

    // -mM: lower and upper boundaries of User-Data
    unsigned min_user_data, max_user_data;

    // -uU: delay between sending/replying to UDR/PNR
    unsigned send_delay, recv_delay;
};
// }}}

// Struct DMXEndPoint {{{
// Binds IP version, address and port in a DMX fashion.
struct DMXEndPoint {
    // The size of output buffer of toString().  It has space
    // for an IPv6 address and a ":65535" port.
    static const size_t STRLEN =
          (sizeof('[')+INET6_ADDRSTRLEN+sizeof(']'))    // [<address>]
        + (sizeof(':')+MAXDIGITS_OF(uint16_t))          // :<port>
        +  sizeof('\0');                                // terminator

    // struct sockaddr utilities
    template <typename T>
        static const struct sockaddr *toCSA(T saddr);
    template <typename T>
        static struct sockaddr *toNSA(T saddr);
    template <typename T>
        static const struct sockaddr_storage *toCSS(T saddr);
    template <typename T>
        static struct sockaddr_storage *toNSS(T saddr);
    template <typename T>
        static const struct sockaddr_in *toCS4(T saddr);
    template <typename T>
        static struct sockaddr_in *toNS4(T saddr);
    template <typename T>
        static const struct sockaddr_in6 *toCS6(T saddr);
    template <typename T>
        static struct sockaddr_in6 *toNS6(T saddr);

    static size_t __attribute__((nonnull))
        sizeOfSA(const struct sockaddr *saddr);
    static const struct sockaddr *__attribute__((nonnull))
        succ(const struct sockaddr *saddr);
    static bool __attribute__((nonnull))
        sameAs(const struct sockaddr *lhs, const struct sockaddr *rhs);
    static char *__attribute((nonnull))
        sockaddrToString(char str[STRLEN], const struct sockaddr *saddr);

    DMXEndPoint() { addr.version = IP_ADDR_T_NOT_DEF_C; }
    DMXEndPoint(const ip_addr_t *a_addr, word a_port = 0):
        addr(*a_addr), port(a_port) NOP()

    // Conversion fro/to struct sockaddr.
    const struct sockaddr_in *__attribute__((nonnull))
        fromSockaddr4(const sockaddr_in *saddr4);
    const struct sockaddr_in6 *__attribute__((nonnull))
        fromSockaddr6(const sockaddr_in6 *saddr6);
    const struct sockaddr *__attribute__((nonnull))
            fromSockaddr(const sockaddr *saddr);
    struct sockaddr *toSockaddr(
                 struct sockaddr_storage *saddr, socklen_t *sizep = NULL,
                 unsigned nmembs = 1, bool isFirst = true) const;
    bool isSockaddr(const struct sockaddr *saddr) const;

    // valid() returns whether the struct holds any address.
    bool valid() const          { return addr.version!=IP_ADDR_T_NOT_DEF_C; }
    byte version() const        { return addr.version;  }
    const byte *ipaddr() const  { return addr.addr;     }

    ip_addr_t addr;
    word port;
}; // }}}

// Struct DGram {{{
// DGrams are what IOTask:s deliver.  It is a variadic size struct
// with some header information and lots of methods.
struct DGram { // {{{
    // DGram:s should be created with alloc() rather than the new operator,
    // but the public constructor is necessary to create compile-time
    // initialized (eg. static constant) DGram:s.
    DGram(size_t size, size_t used = 0):
        // Don't initialize @mData, as the creator is expected to fill it in.
        mTotal(size), mUsed(used), mStreamId(0)
        NOP();

    // Methods {{{
    byte       *begin()                         { return &mData[0]; }
    const byte *begin() const                   { return &mData[0]; }
    byte       *firstUnused()                   { return &mData[mUsed]; }
    const byte *firstUnused() const             { return &mData[mUsed]; }

    size_t      offsetOf(const byte *pos) const { return pos - begin(); }
    byte *      at(size_t offset)               { return &mData[offset]; }
    const byte *at(size_t offset) const         { return &mData[offset]; }
    byte *      translate(const DGram *other, const byte *pos)
                { return at(other->offsetOf(pos)); }
    const byte *translate(const DGram *other, const byte *pos) const
                { return at(other->offsetOf(pos)); }

    size_t      freeSpace() const               { return mTotal - mUsed; }
    bool        empty() const                   { return (mUsed == 0); }
    void        truncate()                      { mUsed = 0; }

    static DGram *alloc(size_t size, DGram *dgram = NULL);
    static bool __attribute__((nonnull(1)))
        expand(DGram **dgramPtr, size_t amount);
    static bool __attribute__((nonnull(1)))
        ensure(DGram **dgramPtr, size_t amount);

    DGram *dupe() const;
    bool __attribute__((nonnull))
        split(DGram **second, const byte *splitAt, size_t reserveSpace = 0);
    DGram *__attribute__((nonnull(1, 1)))
        splice(byte *from, size_t lfrom,
               const char *repl = NULL, size_t lrepl = 0);
    void __attribute((nonnull))
        moveData(byte *to, byte *from, size_t size);
    DGram *__attribute__((nonnull))
        moveSpace(byte *pos, size_t size, byte *from, size_t lfrom);
    // }}}

    // @mTotal:     how many bytes fit in @mData
    // @mUsed:      how many bytes are in @mData
    // @mStreamId:  which SCTP stream has @mData been received from
    //              or on which stream should it be dispatched
    size_t mTotal, mUsed;
    unsigned mStreamId;

    // This needs to be aligned to let DGRAM_FROM_STRING_LITERAL_WITH_SIZE()
    // work.  Interestingly enough DGramTmpl::mPayload is properly unaligned
    // without this clause.
    byte mData[0] __attribute__((aligned));

protected:
    template <typename T>
    static size_t strlen(T str)
        { return ::strlen(CAST(const char *, str)); }
};
// }}}

// Like DGramTmpl except that the @mPayload is fixed @n:umber of @T:hings.
template<typename T, unsigned n>
struct DGramTmplAry: public DGram { // {{{
    // Unless initialized right away consider the array empty,
    // so @mUsed is left 0.
    DGramTmplAry():
        DGram(sizeof(T) * n)
        NOP();
    DGramTmplAry(const T *payload):
        DGram(sizeof(T) * n) {
            memcpy(&mPayload, payload, mTotal);
            mUsed = mTotal;
        }

    T mPayload[n];
}; // }}}
// }}}

// Struct Diameter {{{
// This struct represents a DIAMETER message on wire.  It doesn't add
// any new fields to DGram, only methods to construct and parse DIAMETER
// messages.
struct Diameter: public DGram {
    // Type definitions {{{
    enum {
        // Misc magic numbers
        PROTOCOL_VERSION        = 1,        // DIAMETER header
        ADDR_IPV4               = 1,        // AddressType
        ADDR_IPV6               = 2,        // AddressType
        NO_INBAND_SECURITY      = 0,        // Inband-Security-Id
        REBOOTING               = 0,        // Disconnect-Cause
        DO_NOT_WANT_TO_TALK_TO_YOU = 2,     // Disconnect-Cause
        FUCK_OFF                = DO_NOT_WANT_TO_TALK_TO_YOU,
        CLEARTEXT_DATA          = 46,       // SCTP payload protocol ID
        VENDOR_NSN              = 28458,    // Vendor-Id
        VENDOR_3GPP             = 10415,    // VESA { Vendor-Id }
        TGPP_SH                 = 16777217, // Auth-Application-Id
        NSN_NSE                 = 16777328, // Nokia Service Extension
        IFACE_RF                = 3,        // Base accounting
        IFACE_RO                = 4,        // Credit control
        RELAY                   = 0xffffffff,   // Peer is a relay agent
                                                // which handles all kinds
                                                // of applications.
        REPOSITORY_DATA         = 0,        // Data-Reference
        AUTH_STATE_MAINTAINED   = 0,        // Auth-Session-State

        // Header and AVP flags
#if __BYTE_ORDER == __LITTLE_ENDIAN
        FLAG_REQUEST            = 0x80,
        FLAG_MANDATORY          = 0x40,
        FLAG_ERROR              = 0x20,
        FLAG_VENDOR             = 0x80,
#else
        FLAG_REQUEST            = 1,
        FLAG_MANDATORY          = 2,
        FLAG_ERROR              = 4,
        FLAG_VENDOR             = 1,
#endif

        // Artificial AVP flag used to indicate error during AVP parsing.
        // It's guaranteed to be different from any DIAMETER flags,
        // because those are constrained to be 8-bit.
        FLAG_PARSE_ERROR        = 0x100,

        // Result codes
        RC_SUCCESS              = 2001,
        RC_UNABLE_TO_DELIVER    = 3002,
        RC_UNABLE_TO_COMPLY     = 5012,
        RC_MISSING_AVP          = 5005,

        // Command codes
        CER                     = 257,  // Capability-Exchange
        DWR                     = 280,  // Device-Watchdog
        DPR                     = 282,  // Disconnect-Peer
        UDR                     = 306,  // User-Data
        PNR                     = 309,  // Push-Notification

        // AVP codes
        // Used AVPs
        HOST_IP_ADDR            = 257,  // Address
        ORIGIN_HOST             = 264,  // DiameterIdentity
        ORIGIN_REALM            = 296,  // DiameterIdentity
        VENDOR_ID               = 266,  // Unsigned32 (Enterprise Code)
        PRODUCT_NAME            = 269,  // UTF8String
        FIRMWARE_REVISION       = 267,  // Unsigned32
        SUPPORTED_VENDOR_ID     = 265,  // Unsigned32 (Enterprise Code)
        VENDOR_SPECIFIC_APP_ID  = 260,  // { Vendor-Id, Auth/Acct-App-Id }
        AUTH_APPLICATION_ID     = 258,  // Unsigned32
        ACCT_APPLICATION_ID     = 259,  // Unsigned32
        RESULT_CODE             = 268,  // Unsigned32
        DISCONNECT_CAUSE        = 273,  // Enumerated

        // Recognized AVPs
        SESSION_ID              = 263,  // UTF8String
        ORIGIN_STATE_ID         = 278,  // Unsigned32
        ERROR_MESSAGE           = 281,  // UTF8String
        ERROR_REPORTING_HOST    = 294,  // DiameterIdentity
        DESTINATION_HOST        = 293,  // DiameterIdentity
        DESTINATION_REALM       = 283,  // DiameterIdentity
        FAILED_AVP              = 279,  // { ... }
        EXPERIMENTAL_RESULT     = 297,  // { Vendor-Id, Experimental-RC }
        EXPERIMENTAL_RESULT_CODE = 298, // Unsigned32
        PROXY_INFO              = 284,  // { Proxy-Host, Proxy-State }
        PROXY_HOST              = 280,  // DiameterIdentity
        PROXY_STATE             = 33,   // OctetString
        AUTH_SESSION_STATE      = 277,  // Enumerated (0/1)

        // 3GPP AVPs
        USER_IDENTITY           = 700,  // { Public-Identity, MSISDN }
        PUBLIC_IDENTITY         = 601,  // UTF8String
        MSISDN                  = 701,  // OctetString
        DATA_REFERENCE          = 703,  // Unsigned32
        USER_DATA               = 702,  // OctetString
        SUPPORTED_FEATURES      = 628,  // { Vendor-Id, Feature-List-Id,
                                        //   Feature-List }
        FEATURE_LIST_ID         = 629,  // Unsigned32
        FEATURE_LIST            = 630,  // Unsigned32
        SEND_DATA_INDICATION    = 710,  // Enumerated (Integer32)
        SUBS_REQ_TYPE           = 705,  // Unsigned32
        EXPIRY_TIME             = 709,  // Time (Unsigned32)

        // Grouped AVPs (used by dumpAVP() to know which AVPs to descend into)
        REQUESTED_SERVICE_UNIT  = 437,
        SUBSCRIPTION_ID         = 443,
        USED_SERVICE_UNIT       = 446,
        MULTIPLE_SERVICES_CC    = 456,
        USER_EQUIPMENT_INFO     = 458,
        SERVICE_INFORMATION     = 873,
        PS_INFORMATION          = 874,
        SMS_INFORMATION         = 2000,
    };
    // }}}

    // Constants
    static const unsigned HEADER_SIZE   = sizeof(uint32_t) * 5;
    static const unsigned MIN_AVP_SIZE  = sizeof(uint32_t) * 2;
    static const unsigned MAX_AVP_SIZE  = sizeof(uint32_t) * 3;

    static Diameter *fromDGram(DGram *dgram) {
        return static_cast<Diameter *>(dgram);
    }
    static const Diameter *fromDGram(const DGram *dgram) {
        return static_cast<const Diameter *>(dgram);
    }

    // Methods to create DIAMETER messages and add AVP:s. {{{
    static bool __attribute__((nonnull(1)))
        startMessage(DGram **dgramPtr,
                     unsigned command, unsigned flags,
                     unsigned applicationId,
                     unsigned hopByHop, unsigned endToEnd,
                     size_t *cookiep = NULL);
    static void __attribute__((nonnull))
        finishMessage(DGram *dgram, size_t cookie = 0);

    static bool __attribute__((nonnull(1)))
        createSimpleMessage(DGram **dgramPtr,
                            unsigned cmd, unsigned flags,
                            unsigned hbh, unsigned ete,
                            const character_t *oho, const character_t *ore,
                            unsigned rc);
    static bool __attribute__((nonnull(1)))
        makeResponse(DGram **dgramPtr,
                     bool isError, unsigned resultCode,
                     const char *errorMessage = NULL,
                     const character_t *myOriginHost = NULL,
                     const character_t *myOriginRealm = NULL,
                     size_t maxSize = 0, size_t cookie = 0);

    static bool __attribute__((nonnull))
        addInt32AVP(DGram **dgramPtr, unsigned code, uint32_t n,
                    bool mandatory = true, unsigned vendorId = 0);
    static bool __attribute__((nonnull))
        addStringAVP(DGram **dgramPtr, unsigned code, const char *str,
                     bool mandatory = true, unsigned vendorId = 0);
    static bool __attribute__((nonnull))
        addCharStrAVP(DGram **dgramPtr, unsigned code, const character_t *str,
                      bool mandatory = true, unsigned vendorId = 0);
    static bool __attribute__((nonnull))
        addAddrAVP(DGram **dgramPtr, unsigned code, const ip_addr_t *addr,
                   bool mandatory = true, unsigned vendorId = 0);
    static bool __attribute__((nonnull))
        startAVPGroup(DGram **dgramPtr, unsigned code, size_t *cookiep,
                      bool mandatory = true, unsigned vendorId = 0);
    static void __attribute__((nonnull))
        finishAVPGroup(DGram *dgram, size_t cookie);

    static size_t __attribute__((nonnull))
        writeAVP(DGram *dgram, size_t pos, unsigned code,
                 uint8_t flags, size_t datasize, const void *data,
                 unsigned vendorId = 0);
    static bool __attribute__((nonnull))
        addAVP(DGram **dgramPtr, unsigned code,
               bool mandatory, unsigned vendorId,
               size_t datasize, const void *data);
    // }}}

    // Methods for parsing DIAMETER messages. {{{
    //
    // BIG FUCKING WARNING
    //
    // The nonnull attribute is intentionally not applied to
    // parseMessageHeader(), because otherwise the compiler
    // makes incorrrect presuptions and generates wrong code.
    //
    // In more detail, nonnull(3) declares @msgRemPtr non-NULL.
    // This is so, because the first positional parameter is
    // @this.  However, when enabling inter-procedular-analysis
    // with -fwhole-program the compiler thinks it's @commandPtr
    // is which can't be NULL, and eliminates our if (commandPtr)
    // condition from the subroutine, leading to a segfault when
    // we try to write to that (NULL) memory area the command code
    // of the message.  This is most certainly a compiler bug.
    const byte * // __attribute__((nonnull(3)))
        parseMessageHeader(const byte *pos, size_t *msgRemPtr,
                           unsigned *commandPtr        = NULL,
                           unsigned *flagsPtr          = NULL,
                           unsigned *applicationIdPtr  = NULL,
                           unsigned *hopByHopPtr       = NULL,
                           unsigned *endToEndPtr       = NULL) const;
    const byte * __attribute__((nonnull))
        parseAVPHeader(const byte *pos, size_t *msgRemPtr,
                       unsigned *avpCodePtr, unsigned *flagsPtr,
                       size_t *dataLenPtr, bool checkDataLen = false) const;
    const byte * __attribute__((nonnull))
        parseInt32(const byte *pos, size_t *msgRemPtr,
                   size_t dataLen, uint32_t *valuep) const;
    const byte * __attribute__((nonnull))
        parseString(const byte *pos, size_t *msgRemPtr,
                    size_t dataLen, char **strp) const;
    const byte * __attribute__((nonnull))
        skipAVPData(const byte *pos, size_t *msgRemPtr, size_t dataLen) const;

    static const byte * __attribute__((nonnull))
        isMessageComplete(const DGram *dgram,
                          size_t gapAt = 0, size_t gapSize = 0);

    // These are not in DiaLBS.
    static const byte * __attribute__((nonnull))
        dumpAVP(const Diameter *dia, const byte *pos, size_t *remp,
                unsigned depth = 0);
    static void __attribute__((nonnull(1)))
        dumpMessage(const Diameter *dia, const byte *pos = NULL);
    // }}}

protected: // Methods {{{
    // Methods to mangle bytes in a message.
    static size_t __attribute__((nonnull))
        writeInt32(DGram *dgram, size_t pos, uint32_t data32);
    static size_t __attribute__((nonnull))
        writeInt8_24(DGram *dgram, size_t pos,
                     uint8_t data8, uint32_t data24);
    static size_t __attribute__((nonnull))
        writeInt24(DGram *dgram, size_t pos, uint32_t data24);

    static size_t __attribute__((nonnull))
        writeAVPHeader(DGram *dgram, size_t pos, unsigned code, uint8_t flags,
                       size_t datasize, unsigned vendorId = 0);
    static bool __attribute__((nonnull))
        addAVPHeader(DGram **dgramPtr, unsigned code, bool mandatory,
                     size_t datasize, unsigned vendorId = 0);

    bool __attribute__((nonnull))
        checkSpace(const byte *pos, size_t msgRem, size_t need) const;
    // }}}
}; // }}}
// }}}

// Struct DMXEndPoint {{{
// struct sockaddr type conversion functions {{{
// Cast @saddr to const struct sockaddr *.
template <typename T>
const struct sockaddr *DMXEndPoint::toCSA(T saddr) {
    return CAST(const struct sockaddr *, saddr);
}

// Non-const version.
template <typename T>
struct sockaddr *DMXEndPoint::toNSA(T saddr) {
    return CAST(struct sockaddr *, saddr);
}

// Non-const version.
template <typename T>
struct sockaddr_storage *DMXEndPoint::toNSS(T saddr) {
    return CAST(struct sockaddr_storage *, saddr);
}

// Cast @saddr to const struct sockaddr_in *.
template <typename T>
const struct sockaddr_in *DMXEndPoint::toCS4(T saddr) {
    return CAST(const struct sockaddr_in *, saddr);
}

// Non-const version.
template <typename T>
struct sockaddr_in *DMXEndPoint::toNS4(T saddr) {
    return CAST(struct sockaddr_in *, saddr);
}

// Cast @saddr to const struct sockaddr_in6 *.
template <typename T>
const struct sockaddr_in6 *DMXEndPoint::toCS6(T saddr) {
    return CAST(const struct sockaddr_in6 *, saddr);
}

// Non-const version.
template <typename T>
struct sockaddr_in6 *DMXEndPoint::toNS6(T saddr) {
    return CAST(struct sockaddr_in6 *, saddr);
}
// }}}

// Returns the size of the specific struct sockaddr_* of @saddr.
size_t DMXEndPoint::sizeOfSA(const struct sockaddr *saddr) {
    if (saddr->sa_family == AF_INET)
        return sizeof(struct sockaddr_in);
    else if (saddr->sa_family == AF_INET6)
        return sizeof(struct sockaddr_in6);
    else
        return sizeof(struct sockaddr);
}

// Returns the sockaddr following @saddr, taking its address family into
// account.
const struct sockaddr *DMXEndPoint::succ(const struct sockaddr *saddr) {
    return toCSA(CAST(const char *, saddr) + sizeOfSA(saddr));
}

// Returns whether @lhs == @rhs.
bool DMXEndPoint::sameAs(const struct sockaddr *lhs,
                         const struct sockaddr *rhs) {
    // It seems to be a little slippery slope we're doing here, because
    // gcc complains aboud violating strict aliasing rules if we use the
    // == operator to compare integers.  Use memcmp() instead.
    //
    // Compare the address families.
    if (memcmp(&lhs->sa_family, &rhs->sa_family, sizeof(lhs->sa_family)))
        return false;

    if (lhs->sa_family == AF_INET) {
        const struct sockaddr_in *lhs4, *rhs4;

        lhs4 = toCS4(lhs);
        rhs4 = toCS4(rhs);

        // Compare the IPv4 addresses.
        if (memcmp(&lhs4->sin_addr, &rhs4->sin_addr,
                   sizeof(lhs4->sin_addr)))
            return false;

        // Compare the ports.
        return !memcmp(&lhs4->sin_port, &rhs4->sin_port,
                       sizeof(lhs4->sin_port));
    } else if (lhs->sa_family == AF_INET6) {
        const struct sockaddr_in6 *lhs6, *rhs6;

        lhs6 = toCS6(lhs);
        rhs6 = toCS6(rhs);
        if (memcmp(&lhs6->sin6_addr, &rhs6->sin6_addr,
                   sizeof(lhs6->sin6_addr)))
            return false;
        return !memcmp(&lhs6->sin6_port, &rhs6->sin6_port,
                       sizeof(lhs6->sin6_port));
    } else
        return !memcmp(lhs, rhs, sizeof(*lhs));
}

// Prints @saddr to @str and returns it, preserving @errno.
char *DMXEndPoint::sockaddrToString(char str[STRLEN],
                                    const struct sockaddr *saddr) {
    int serrno;

    serrno = errno;
    if (saddr->sa_family == AF_INET) {
        const struct sockaddr_in *saddr4 = toCS4(saddr);
        inet_ntop(saddr4->sin_family, &saddr4->sin_addr,
                  str, INET_ADDRSTRLEN);
        sprintf(str + strlen(str), ":%hu", ntohs(saddr4->sin_port));
    } else if (saddr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *saddr6 = toCS6(saddr);
        char *p;

        p = str;
        *p++ = '[';
        p += strlen(inet_ntop(saddr6->sin6_family, &saddr6->sin6_addr,
                    p, INET6_ADDRSTRLEN));
        *p++ = ']';
        sprintf(p, ":%hu", ntohs(saddr6->sin6_port));
    } else
        strcpy(str, "<unknown>");
    errno = serrno;

    return str;
}

// Initialize @addr and @port from a struct sockaddr_in and returns
// a pointer to the next structure.
const struct sockaddr_in *DMXEndPoint::fromSockaddr4(
                                               const sockaddr_in *saddr4) {
    FILL_STRUCT(addr.addr);
    addr.version = IP_ADDR_T_IPV4_C;
    addr.addr[0] =  saddr4->sin_addr.s_addr        & 0xFF;
    addr.addr[1] = (saddr4->sin_addr.s_addr >>  8) & 0xFF;
    addr.addr[2] = (saddr4->sin_addr.s_addr >> 16) & 0xFF;
    addr.addr[3] = (saddr4->sin_addr.s_addr >> 24) & 0xFF;
    port         = ntohs(saddr4->sin_port);
    return ++saddr4;
}

// Initialize @addr and @port from a struct sockaddr_in6 and returns
// a pointer to the next structure.
const struct sockaddr_in6 *DMXEndPoint::fromSockaddr6(
                                              const sockaddr_in6 *saddr6) {
    FILL_STRUCT(addr.addr);
    addr.version = IP_ADDR_T_IPV6_C;
    memcpy(addr.addr, saddr6->sin6_addr.s6_addr,
            sizeof(saddr6->sin6_addr.s6_addr));
    port         = ntohs(saddr6->sin6_port);
    return ++saddr6;
}

// Initialize @addr and @port from @saddr and return a pointer to the
// next sockaddr or returtns the original @saddr in case DMXEndPoint
// couldn't handle the socket address within.
const struct sockaddr *DMXEndPoint::fromSockaddr(const sockaddr *saddr) {
    if (saddr->sa_family == AF_INET)
        return toCSA(fromSockaddr4(toCS4(saddr)));
    else if (saddr->sa_family == AF_INET6)
        return toCSA(fromSockaddr6(toCS6(saddr)));
    else
        return saddr;
}

// Convert this struct to sockaddr_in or sockaddr_in6 given in @storage.
// You can also use this function to convert an array of DMXEndPoint:s
// to a tightly packed array of sockadrr_in:s and sockaddr_in6:es
// if you tell the number of array members in @nmembs.  The only failure
// condition is out of memory (or meeting unknown IP protocol versions).
// You can supply @saddr if you're certain it will be large enough,
// or you can leave it NULL, in which case a new suitably sized sockaddr
// will be allocated and returned.
struct sockaddr *DMXEndPoint::toSockaddr(struct sockaddr_storage *saddr,
                   socklen_t *sizep, unsigned nmembs, bool isFirst) const {
    socklen_t size;
    struct sockaddr_storage *next;

    // Write our data to @saddr.
    next = NULL;
    if (addr.version == IP_ADDR_T_IPV4_C) {
        struct sockaddr_in *saddr4;

        size = sizeof(*saddr4);
        if (!saddr)
            goto out;

        // Write s_addr directly in network byte order.
        saddr4 = toNS4(saddr);
        memset(saddr4, 0, sizeof(*saddr4));
        saddr4->sin_family = AF_INET;
        saddr4->sin_addr.s_addr =   (ipaddr()[3] << 24)
                                  | (ipaddr()[2] << 16)
                                  | (ipaddr()[1] <<  8)
                                  |  ipaddr()[0];
        saddr4->sin_port = htons(port);
    } else if (addr.version == IP_ADDR_T_IPV6_C) {
        struct sockaddr_in6 *saddr6;

        size = sizeof(*saddr6);
        if (!saddr)
            goto out;

        saddr6 = toNS6(saddr);
        memset(saddr6, 0, sizeof(*saddr6));
        saddr6->sin6_family = AF_INET6;
        memcpy(saddr6->sin6_addr.s6_addr, ipaddr(),
               sizeof(saddr6->sin6_addr.s6_addr));
        saddr6->sin6_port = htons(port);
    } else {
        size = sizeof(struct sockaddr);
        if (!saddr)
            goto out;

        memset(saddr, 0, sizeof(struct sockaddr));
        toNSA(saddr)->sa_family = AF_UNSPEC;
    }

    // Love you, C++!
    next = toNSS(const_cast<struct sockaddr *>(succ(toNSA(saddr))));

out:
    if (nmembs > 1) {
        socklen_t nextSize;

        // There is/are more addresses to be appended to @saddr.
        (this+1)->toSockaddr(next, &nextSize, nmembs - 1, false);
        size += nextSize;
        if (sizep)
            *sizep = nextSize;
    } else if (sizep)
        *sizep = size;

    if (isFirst && !saddr) {
        // This is the outermost invocation of this function and we must
        // allocate @saddr.  The needed buffer @size is already determined.
        if (!(saddr = toNSS(malloc(size)))) {
            ERR("malloc(%d): %m", size);
            return NULL;
        }
        toSockaddr(saddr, NULL, nmembs, false);
    }

    return toNSA(saddr);
}

// Returns whether @saddr conveys the same information as this object.
bool DMXEndPoint::isSockaddr(const struct sockaddr *saddr) const {
    struct sockaddr_storage me;
    char addrstr[DMXEndPoint::STRLEN];

    toSockaddr(&me);
    DBG("comparing against %s", sockaddrToString(addrstr, toCSA(&me)));
    return sameAs(saddr, toCSA(&me));
}
// }}}

// Struct DGram {{{
// Allocate or change the capacity of @dgram.  Returns a pointer
// to the new location of the DGram or NULL on failure.
DGram *DGram::alloc(size_t size, DGram *dgram) {
    DGram *newDGram;
    size_t newTotal;

    newTotal = size;
    size += sizeof(*dgram);
    if (!(newDGram = static_cast<DGram *>(realloc(
                                  static_cast<void *>(dgram), size)))) {
        ERR("realloc(%zu): %m", size);
        return NULL;
    } else if (dgram) {
        newDGram->mTotal = newTotal;
        if (newDGram->mUsed > newTotal)
            newDGram->mUsed = newTotal;
        // Caller is responsible for the contents of the DGram.
    } else {
        memset(newDGram, 0, sizeof(*newDGram));
        newDGram->mTotal = newTotal;
    }

    return newDGram;
}

// Allocate a new DGram or increase *dgramPtr's capacity by @amount.
// *dgramPtr is only overwritten on success.  Returns whether the
// allocation was successful.
bool DGram::expand(DGram **dgramPtr, size_t amount) {
    DGram *dgram;
    size_t newSize;

    DIAASSERT(dgramPtr != NULL);

    newSize  = *dgramPtr ? (*dgramPtr)->mTotal : 0;
    newSize += amount;

    if ((dgram = alloc(newSize, *dgramPtr)) != NULL) {
        *dgramPtr = dgram;
        return true;
    } else
        return false;
}

// Ensure that *dgramPtr has at least @needed bytes of freeSpace().
// Returns whether allocation was successful.
bool DGram::ensure(DGram **dgramPtr, size_t needed) {
    DIAASSERT(dgramPtr);
    DGram *dgram = *dgramPtr;

    if (!dgram) {
        if (!(dgram = alloc(needed)))
            return false;
        *dgramPtr = dgram;
    } else if (needed > dgram->freeSpace())
        return expand(dgramPtr, needed - dgram->freeSpace());
    return true;
}

// Return a slim duplicate of this DGram with only @mUsed bytes of capacity.
DGram *DGram::dupe() const {
    DGram *dgram;

    if (!(dgram = alloc(mUsed)))
        return NULL;

    memcpy(dgram, this, sizeof(*this) + mUsed);
    dgram->mTotal = mUsed;

    return dgram;
}

// Split a DGram into two halves at @splitAt.  If @splitAt points right
// outside the used area of @mData, nothing is allocated and *second is
// set to NULL.  Otherwise the head of the DGram remains unaltered, but
// the tail is returned in *second, with @reserveSpace free space at the
// end.  If the allocation fails, false is returned.
bool DGram::split(DGram **second, const byte *splitAt, size_t reserveSpace) {
    size_t offset, secondSize;

    // Input pointers must be valid.
    DIAASSERT(second);
    DIAASSERT(splitAt);

    // @atptr must point to valid offset in @mData.
    DIAASSERT(splitAt >= mData);
    DIAASSERT(splitAt <= firstUnused());

    offset = splitAt - mData;
    if (offset >= mUsed) {
        // DGram should be split at the end.  We're ready.
        *second = NULL;
        return true;
    }

    // DGram should be split at the beginning or somewhere in the middle.
    secondSize = mUsed - offset;
    if (!(*second = DGram::alloc(secondSize + reserveSpace)))
        return false;

    memcpy((*second)->mData, &mData[offset], secondSize);
    (*second)->mUsed = secondSize;

    mUsed = offset;
    return true;
}

// Replace the range [@from..@from+@lfrom[ with an @lrepl-sized section,
// and optionally copy @repl there.  @from can point at firstUnused(),
// in which case the DGram will be extended by @lfrom.
DGram *DGram::splice(byte *from, size_t lfrom,
                     const char *repl, size_t lrepl) {
    DGram *dgram = this;
    size_t fromIdx, ldiff;

    DIAASSERT(begin() <= from && from + lfrom <= firstUnused());
    fromIdx = offsetOf(from);

    // Shrink or expand @dgram?
    if (lrepl < lfrom) {
        // begin()        from      lrepl       lfrom       mUsed
        // |--------------|---------|///////////|-----------|
        ldiff = lfrom - lrepl;
        memmove(from+lrepl, from+lfrom, (mUsed-fromIdx) - lfrom);
        mUsed -= ldiff;
    } else if (lrepl > lfrom) {
        // begin()        from      lfrom       lrepl       mUsed
        // |--------------|---------|+++++++++++|-----------|
        ldiff = lrepl - lfrom;
        if (!DGram::ensure(&dgram, ldiff))
            return NULL;
        from = dgram->at(fromIdx);
        memmove(from+lrepl, from+lfrom, (dgram->mUsed-fromIdx) - lfrom);
        dgram->mUsed += ldiff;
    }

    // Fill up the new section.
    if (repl)
        memcpy(from, repl, lrepl);
    return dgram;
}

// Cut [@from..@from+@size[ and paste it @to.  The section
// must be within the DGram.  See the second diagram for illustration.
void DGram::moveData(byte *to, byte *from, size_t size) {
    DIAASSERT(begin() <= from && &from[size] <= firstUnused());
    DIAASSERT(begin() <= to   && &to[  size] <= firstUnused());
    if (!size)
        return;

    if (to > from) {
        //        +from    +from+size
        // |000000aaaaaaaaabbbbbbbbbbcccccccccccccc1111111|
        // |000000bbbbbbbbbbccccaaaaaaaaccccccccccc1111111|
        //                      +to
        // moveData(from, from+size, to-from);
        size_t newsize = to - from;
        to = from;
        from += size;
        size = newsize;
    }

    //        +to                +from         +from+size
    // |000000aaaaaaaaabbbbbbbbbBcccccccccccccc1111111|
    // |000000ccccccccccccccaaaaaaaaabbbbbbbbbB1111111|
    while (from > to) {
        byte tmp;
        byte *src, *dst;

        // Move [@from..@from+@size[ left by 1 byte.
        src = from;
        dst = from - 1;
        tmp = *dst;
        while (src < from + size)
            *dst++ = *src++;
        *dst = tmp;
        from--;
    } // until all 'c's are in place
}

// Delete @lfrom-byte space @from the DGram, and punch a @size bytes hole
// at @pos.  Either @lfrom or @size can be 0, but both @from and @pos must
// be within the DGram.  The punched DGram can either be smaller or larger
// than it was.  See the diagrams below for a more comprehensible description.
//
// Although what this function does could be achieved by two splice()s,
// the point is to do it with as few allocations and memmove()s as possible.
//
// One gotcha of this implementation is that if expansion fails, the contents
// of the DGram may be trashed.
DGram *DGram::moveSpace(byte *pos, size_t size, byte *from, size_t lfrom) {
    DGram *dgram = this;
    size_t idx = dgram->offsetOf(pos);

    DIAASSERT(begin() <= from && &from[lfrom] <= firstUnused());
    DIAASSERT(begin() <= pos  && &pos[ lfrom] <= firstUnused());

    if (size == lfrom) {
        // Since the source and the destination holes are equally wide,
        // a single memmove() is enough.
        if (!size)
            /* NOP */;
        else if (pos < from)
            //      +pos                 +from
            // [----+>>>>>>>>>>>>>>>>>>>>| size |--------------] // before
            // [----| size |>>>>>>>>>>>>>>>>>>>>>--------------] // after
            memmove(&pos[size], pos, from - pos);
        else if (pos > from)
            //      +from   +from+lfrom    +pos   +pos+size
            // [----| size |<<<<<<<<<<<<<<<<<<<<<<<------------] // before
            // [----<<<<<<<<<<<<<<<<<<<<<<<| size |------------] // after
            // length(<) == pos+size - (from+lfrom) == pos+size - (from+size)
            //           == pos+size - from-size == pos - from
            memmove(from, &from[lfrom], pos - from);
    } else if (pos <= from) {
        //      +pos                 +from
        // [----+>>>>>>>>>>>>>>>>>>>>| lfrom |-------------] // before
        // [----+>>>>>>>>>>>>>>>>>>>>| size |-------------]  // splice()
        // [----| size |>>>>>>>>>>>>>>>>>>>>>-------------]  // after
        //
        // Overlapping case:      +pos
        //                        +  +from
        // [----------------------+>>| lfrom |-------------] // before
        // [----------------------+>>| size |-------------]  // splice()
        // [----------------------| size |>>--------------]  // after
        //
        // Grow/shrink <from>, then shift bytes right between @pos and @from.
        size_t diff = from - pos; // == length(>)
        if (!(dgram = splice(from, lfrom, NULL, size)))
            return NULL;
        pos = dgram->at(idx);
        memmove(&pos[size], pos, diff);
    } else { // @pos > @from
        //     +from    +from+lfrom    +pos     +pos+lfrom
        // [---| lfrom |<<<<<<<<<<<<<<<<<<<<<<<<-----------] // before
        // [---<<<<<<<<<<<<<<<<<<<<<<<<| lfrom |-----------] // memmove()
        // [---<<<<<<<<<<<<<<<<<<<<<<<<| size |-----------]  // after
        //
        // Overlapping case:
        //                          +pos+lfrom
        //     +from   +pos +from+lfrom
        // [---| l f r o m |<<<<<<<<+----------------------] // before
        // [---<<<<<<<<| l f r o m |-----------------------] // memmove()
        // [---<<<<<<<<| size |-----------------------]      // after
        //
        // Shift bytes left so that @pos will be <from>-wide,
        // then make this hole smaller or larger.
        // length(<) == (pos+lfrom) - (from+lfrom) == pos - from
        memmove(from, &from[lfrom], pos - from);
        dgram = splice(pos, lfrom, NULL, size);
    }

    return dgram;
}
// }}}

// Struct Diameter {{{
// Private methods {{{
// Check whether both the DIAMETER message and its containing DGram
// has enough length and capacity for additional @need bytes.
// This is typically used by the parser to ensure that the next field
// of @need:ed size (eg. an integer) can be part of the message.
bool Diameter::checkSpace(const byte *pos, size_t msgRem,
                          size_t need) const {
    DIAASSERT(pos);

    if (msgRem < need) {
        ERR("Partial message at offset %zu (remained %zu, needs %zu bytes)",
            offsetOf(pos), msgRem, need);
        return false;
    } else if (firstUnused() < pos + need) {
        ERR("Truncated message at offset %zu "
            "(available %tu, needed %zu bytes)",
            offsetOf(pos), firstUnused() - pos, need);
        return false;
    } else
        return true;
}

// Write htonl(@data32) at @pos in @dgram.  Returns the index of the first
// byte after the place @data32 has been written to.  @dgram is expected
// to have sufficient freeSpace().
size_t Diameter::writeInt32(DGram *dgram, size_t pos, uint32_t data32) {
    // Verify that we're about to write within @mTotal and that the start
    // of writing is not outside of @mUsed.
    DIAASSERT(dgram != NULL);
    DIAASSERT(pos <= dgram->mUsed);
    DIAASSERT(pos + sizeof(data32) <= dgram->mTotal);

    data32 = htonl(data32);
    memcpy(&dgram->mData[pos], &data32, sizeof(data32));

    // Extend @mUsed if we've written beyond it.
    pos += sizeof(data32);
    if (dgram->mUsed < pos)
        dgram->mUsed = pos;
    return pos;
}

// Write 1 byte followed by 3 bytes of integer (converted to NBO)
// at @pos in @dgram.
size_t Diameter::writeInt8_24(DGram *dgram, size_t pos,
                              uint8_t data8, uint32_t data24) {
    size_t newPos = writeInt32(dgram, pos, data24);
    dgram->mData[pos] = data8;
    return newPos;
}

// Update the 3 bytes portion of a dword at @pos.
size_t Diameter::writeInt24(DGram *dgram, size_t pos, uint32_t data24) {
    return writeInt8_24(dgram, pos, dgram->mData[pos], data24);
}

// Write an AVP header at @pos.  @datasize should _not_ be the rounded
// size of payload-to-be-added.
size_t Diameter::writeAVPHeader(DGram *dgram, size_t pos,
                                unsigned code, uint8_t flags,
                                size_t datasize, unsigned vendorId) {
    pos = writeInt32(dgram, pos, code);
    if (vendorId) {
        pos =  writeInt8_24(dgram, pos, flags, MAX_AVP_SIZE + datasize);
        return writeInt32(dgram, pos, vendorId);
    } else
        return writeInt8_24(dgram, pos, flags, MIN_AVP_SIZE + datasize);
}

// Write a complete AVP at @pos.
size_t Diameter::writeAVP(DGram *dgram, size_t pos,
                          unsigned code, uint8_t flags,
                          size_t datasize, const void *data,
                          unsigned vendorId) {
    // Write the header...
    pos = writeAVPHeader(dgram, pos, code, flags, datasize, vendorId);

    // ...the payload...
    DIAASSERT(pos + datasize <= dgram->mTotal);
    memcpy(&dgram->mData[pos], data, datasize);
    pos += datasize;

    // ...and the padding.
    size_t pad = PAD4(datasize);
    DIAASSERT(pos + pad <= dgram->mTotal);
    memset(&dgram->mData[pos], 0, pad);
    pos += pad;

    // Update @mUsed.
    if (dgram->mUsed < pos)
        dgram->mUsed = pos;
    return pos;
}

// Add an AVP header to *dgramPtr.
bool Diameter::addAVPHeader(DGram **dgramPtr, unsigned code, bool mandatory,
                            size_t datasize, unsigned vendorId) {
    unsigned flags;
    size_t headerSize;

    flags = 0;
    headerSize = MIN_AVP_SIZE;
    if (mandatory)
        flags |= FLAG_MANDATORY;
    if (vendorId) {
        flags |= FLAG_VENDOR;
        headerSize += sizeof(uint32_t);
    }

    if (!ensure(dgramPtr, headerSize))
        return false;

    // @dgram->mUsed will be extended by writeAVPHeader() and beneath.
    writeAVPHeader(*dgramPtr, (*dgramPtr)->mUsed, code, flags, datasize,
                   vendorId);

    return true;
}

// Add an AVP with @data to *dgramPtr.  For reference, an AVP
// looks like this:
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                           AVP Code                            |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V M P r r r r r|                  AVP Length                   |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                        Vendor-ID (opt)                        |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |    Data ...
// +-+-+-+-+-+-+-+-+
bool Diameter::addAVP(DGram **dgramPtr, unsigned code,
                      bool mandatory, unsigned vendorId,
                      size_t datasize, const void *data) {
    DGram *dgram;
    unsigned flags;
    size_t headerSize;

    flags = 0;
    headerSize = MIN_AVP_SIZE;
    if (mandatory)
        flags |= FLAG_MANDATORY;
    if (vendorId) {
        flags |= FLAG_VENDOR;
        headerSize += sizeof(uint32_t);
    }

    if (!ensure(dgramPtr, headerSize + ALIGN4(datasize)))
        return false;
    dgram = *dgramPtr;

    writeAVP(dgram, dgram->mUsed, code, flags, datasize, data, vendorId);
    DIAASSERT(dgram->mUsed % sizeof(uint32_t) == 0);

    return true;
}
// }}}

// Message construction {{{
// Add an Integer32-type AVP to *dgramPtr.
bool Diameter::addInt32AVP(DGram **dgramPtr, unsigned code, uint32_t n,
                           bool mandatory, unsigned vendorId) {
    n = htonl(n);
    return addAVP(dgramPtr, code, mandatory, vendorId, sizeof(n), &n);
}

// Add an OctetString-type AVP to *dgramPtr.
bool Diameter::addStringAVP(DGram **dgramPtr, unsigned code, const char *str,
                            bool mandatory, unsigned vendorId) {
    return addAVP(dgramPtr, code, mandatory, vendorId, strlen(str), str);
}

// Add an OctetString-type AVP to *dgramPtr.
bool Diameter::addCharStrAVP(DGram **dgramPtr,
                             unsigned code, const character_t *str,
                             bool mandatory, unsigned vendorId) {
    return addStringAVP(dgramPtr, code, CAST(const char *, str),
                        mandatory, vendorId);
}

// Add an Address-type AVP to *dgramPtr.
bool Diameter::addAddrAVP(DGram **dgramPtr,
                          unsigned code, const ip_addr_t *addr,
                          bool mandatory, unsigned vendorId) {
    // This is how a network address is represented according to RFC 4.3.1.
    // It's remarkably similar to ip_addr_t, but unfortunately the size of
    // @version differs.
    struct
    {
        uint16_t version;
        char addr[16];
    } daddr;
    size_t sdaddr;

    if (addr->version == IP_ADDR_T_IPV4_C) {
        daddr.version = htons(ADDR_IPV4);
        sdaddr = 4;
    } else {
        daddr.version = htons(ADDR_IPV6);
        sdaddr = 16;
    }

    memcpy(daddr.addr, addr->addr, sdaddr);
    sdaddr += sizeof(daddr.version);

    return addAVP(dgramPtr, code, mandatory, vendorId, sdaddr, &daddr);
}

// Start an AVP group.  You should preserve *cookiep and present it
// to finishAVPGroup().
bool Diameter::startAVPGroup(DGram **dgramPtr, unsigned code, size_t *cookiep,
                             bool mandatory, unsigned vendorId) {
    DIAASSERT(dgramPtr != NULL && *dgramPtr != NULL);
    DIAASSERT(cookiep != NULL);
    *cookiep = (*dgramPtr)->mUsed;

    return addAVPHeader(dgramPtr, code, mandatory, 0, vendorId);
}

// Close an AVP group.
void Diameter::finishAVPGroup(DGram *dgram, size_t cookie) {
    // @cookie points to the start of the AVP group.
    // We want to finalize that AVP's size.
    writeInt24(dgram, cookie + sizeof(uint32_t), dgram->mUsed - cookie);
}

// Start a DIAMETER message by writing its header.  All values should be
// in host byte order.  If you're writing more than one messages,
// you should preserve *cookiep and present it to finishMessage().
// When writing the first message, this is not important.
//
// As you're building the message you should always check the return
// value of the methods.
//
// For reference, a DIAMETER header looks like this:
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |    Version    |                 Message Length                |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// | Command Flags |                  Command Code                 |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                         Application-ID                        |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                      Hop-by-Hop Identifier                    |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                      End-to-End Identifier                    |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
bool Diameter::startMessage(DGram **dgramPtr,
                            unsigned command, unsigned flags,
                            unsigned applicationId,
                            unsigned hopByHop, unsigned endToEnd,
                            size_t *cookiep) {
    if (!ensure(dgramPtr, HEADER_SIZE))
        return false;
    DGram *dgram = *dgramPtr;
    if (cookiep)
        *cookiep = dgram->mUsed;

    // Skip version and msgsize.
    dgram->mUsed += sizeof(uint32_t);
    writeInt8_24(dgram, dgram->mUsed, flags, command);
    writeInt32(  dgram, dgram->mUsed, applicationId);
    writeInt32(  dgram, dgram->mUsed, hopByHop);
    writeInt32(  dgram, dgram->mUsed, endToEnd);

    return true;
}

// Finish a DIAMETER message by finalizing its header.
void Diameter::finishMessage(DGram *dgram, size_t cookie) {
    // Write the final message length to the message header.
    writeInt8_24(dgram, cookie, PROTOCOL_VERSION, dgram->mUsed);
    DIAASSERT(dgram->mUsed % sizeof(uint32_t) == 0);
}

// Create a simple DIAMETER message in @dgramPtr with the indicated
// header-values, Origin-Host, Origin-Realm and Result-Code.
bool Diameter::createSimpleMessage(DGram **dgramPtr,
                               unsigned cmd, unsigned flags,
                               unsigned hbh, unsigned ete,
                               const character_t *oho, const character_t *ore,
                               unsigned rc) {
    if (*dgramPtr)
        (*dgramPtr)->truncate();
    // DIAMETER header + 3 AVP + 2*20 strings + 1 spare AVP.
    else if (!(*dgramPtr = DGram::alloc(HEADER_SIZE + MAX_AVP_SIZE*4 + 20*2)))
        return false;

    if (!Diameter::startMessage(dgramPtr, cmd, flags, 0, hbh, ete))
        return false;
    if (oho && !Diameter::addCharStrAVP(dgramPtr, ORIGIN_HOST, oho))
        return false;
    if (ore && !Diameter::addCharStrAVP(dgramPtr, ORIGIN_REALM, ore))
        return false;
    if (!(flags & Diameter::FLAG_REQUEST)
        && !Diameter::addInt32AVP(dgramPtr, RESULT_CODE,
                                  rc ? rc : (unsigned)RC_SUCCESS))
        return false;
    Diameter::finishMessage(*dgramPtr);
    return true;
}

// Create an error response complying with Section 6.2. (Diameter Answer
// Processing) and Section 7.2 (Error Bit) of the RFC:
// -- clear the request flag
// -- set the error flag if @isError
// -- preserve the P bit
// -- keep the Command-Code, Application-ID, Hop-by-Hop and End-to-End ID
//    (The provided @appId is only used as a fallback if @dgram couldn't be
//     parsed at all.)
// -- replace Origin-Host and Origin-Realm with the provided values
//    if it's a request
// -- add a Result-Code, which must be given
// -- preserve Session-Id and Proxy-Info
// -- drop Destination-Host, Destination-Realm and Origin-State-Id
//
// If @maxSize > 0 the resulting @dgram won't be let grow larger than that,
// and it will be truncated at AVP boundary, so it remains a valid Diameter
// message.
bool Diameter::makeResponse(DGram **dgramPtr,
                            bool isError, unsigned resultCode,
                            const char *errorMessage,
                            const character_t *myOriginHost,
                            const character_t *myOriginRealm,
                            size_t maxSize, size_t cookie) {
    DGram *dgram;
    const Diameter *dia;
    unsigned i;
    unsigned avp, avpFlags;
    unsigned appId, msgFlags, cmd, hbh, ete;
    size_t msgSize, rem, dataLen, deleteSize;
    byte *top, *pos, *deleteFrom;
    const byte *nextPos;

    // AVPs whose relative position is significant in the DIAMETER message.
    // Note that the standard doesn't mandate that this ordering must be
    // followed, but surely it doesn't hurt interoperability and diagnostics.
    struct {
        unsigned avp;
        unsigned seen;  // The number of times we've seen this AVP.
    } avpTbl[] = {  // All unspecified fields are zeroed.
        { SESSION_ID            },
        { ORIGIN_HOST           },
        { ORIGIN_REALM          },
        { RESULT_CODE           },
        { ERROR_MESSAGE         },
        { ERROR_REPORTING_HOST  },
        { FAILED_AVP            },
        { EXPERIMENTAL_RESULT   },
        { PROXY_INFO            },
    };

    // { entryOf(avpTbl), size } vector to track the current ordering
    // of the AVPs in the @dgram.
    std::vector<std::pair<unsigned, size_t> > avps;

    // It's important to initialize these early because we can end up
    // in fatal very soon.
    msgFlags = cmd = hbh = ete = 0;

    // Verify that we can write @msgFlags in @dgram.
    dgram = *dgramPtr;
    DIAASSERT(resultCode != 0);
    DIAASSERT(cookie < dgram->mUsed);
    if (dgram->mUsed < cookie + sizeof(uint32_t) + sizeof(uint8_t)) {
        LOG("message too short (%zu bytes)", dgram->mUsed-cookie);
        goto fatal;
    }

    // Set the message flags.
    dgram->mData[cookie + sizeof(uint32_t)] &= ~FLAG_REQUEST;
    if (isError)
        dgram->mData[cookie + sizeof(uint32_t)] |=  FLAG_ERROR;
    else
        dgram->mData[cookie + sizeof(uint32_t)] &= ~FLAG_ERROR;

    // Parse the message header.  We leave Command-Code, Application-ID,
    // Hop-by-Hop and End-to-End IDs as were.
    dia = fromDGram(dgram);
    if (!(top = (byte *)dia->parseMessageHeader(&dgram->mData[cookie],
                                                &msgSize, &cmd, &msgFlags,
                                                &appId, &hbh, &ete)))
        goto fatal;
    else if (top == dgram->at(cookie))
        goto fatal;

    // Go through the AVP:s and take note of the positions of all the
    // known ones in order to be able to rearrange them properly.
    // Also delete all Destination-* and redundant AVPs right here.
    rem = msgSize;
    deleteFrom = NULL;
    deleteSize = 0;
    for (pos = top;
         (nextPos = dia->parseAVPHeader(pos, &rem, &avp, &avpFlags,
                                        &dataLen, true)) != NULL;
         pos = const_cast<byte *>(nextPos)) {
        // @avpSize := size of AVP header + rounded @dataLen
        size_t avpSize = (nextPos - pos) + ALIGN4(dataLen);
        if (avp == DESTINATION_HOST || avp == DESTINATION_REALM
            || (avp == ORIGIN_STATE_ID && myOriginHost)) {
            // These AVPs are to be deleted.
            if (!deleteFrom)
                // This is the first AVPs in a series to be deleted.
                deleteFrom = pos;
            deleteSize += avpSize;
        } else { // @avp != Destination-*
            // Is this a known AVP?
            for (i = 0; i < MEMBS_OF(avpTbl) && avpTbl[i].avp != avp; i++)
                ;

            // Keep this AVP?
            if (i >= MEMBS_OF(avpTbl)
                || !avpTbl[i].seen || avp == PROXY_INFO) {
                // Yeah.  If there's anything to delete, it's time to do it.
                if (deleteFrom) {
                    /*
                     * Yes, delete:
                     *             /avpTbl[0].size\
                     * aaaaaaaaaaaabbbbbbbbbbbbbbbbbccccccddddddddddddd
                     *             ^avpTbl[0].idx   ^pos  ^nextPos
                     */
                    dgram->splice(deleteFrom, deleteSize);
                    // Since we shrinked @dgram, splice() cannot fail,
                    // and @dgram remains valid.

                    // Adjust @pos and @nextPos according to the figure above
                    // and reduce @msgSize.
                    DIAASSERT(msgSize >= deleteSize);
                    pos -= deleteSize;
                    nextPos -= deleteSize;
                    msgSize -= deleteSize;
                    DIAASSERT(pos > dgram->begin());
                    DIAASSERT(pos + avpSize <= dgram->firstUnused());
                    DIAASSERT(nextPos > dgram->begin());

                    // Finished deleting this block.
                    deleteFrom = NULL;
                    deleteSize = 0;
                } // AVPs deleted

                // Can we merge up with the avps.last()?
                if (avps.size() && avps.back().first == i) {
                    // The previous AVP was of the same kind as this.
                    avps.back().second += avpSize;
                } else {
                    avps.push_back(std::make_pair(i, avpSize));
                    if (i < MEMBS_OF(avpTbl))
                        avpTbl[i].seen++;
                }
            } else {
                // This is a redundant AVP (eg. a second Session-Id),
                // therefore it will be deleted.
                if (!deleteFrom)
                    deleteFrom = pos;
                deleteSize += avpSize;
            }
        } // @avp != Destination-*

        if (!(nextPos = dia->skipAVPData(nextPos, &rem, dataLen)))
            // Not supposed to happen.
            goto fatal;
    } // for each AVPs in @dgram

    // Truncate the last AVP:s starting from @pos if there was a parse error.
    if (avpFlags & FLAG_PARSE_ERROR) {
        if (deleteFrom)
            DIAASSERT(deleteFrom+deleteSize == pos);
        dgram->mUsed = dgram->offsetOf(pos);
        msgSize = dgram->mUsed-cookie;
        if (deleteFrom)
            DIAASSERT(deleteFrom+deleteSize == dgram->firstUnused());
    }

    if (deleteFrom) {
        // The last AVPs are to be deleted.  Simply shrink the @dgram.
        DIAASSERT(dgram->mUsed > deleteSize);
        DIAASSERT(msgSize >= deleteSize);
        dgram->mUsed -= deleteSize;
        msgSize -= deleteSize;
        DIAASSERT(deleteFrom == dgram->firstUnused());
    }

    // Go through @avpTbl and bring them into physical order, as recommended
    // in the standard.  Also replace Origin-* and Result-Code here.  @top
    // designates the position up until the DGram is OK, and at which the
    // next AVP can be inserted.  Initially it points to the first AVP.
    for (i = 0; i < MEMBS_OF(avpTbl);
         avpTbl[i].seen ? avpTbl[i].seen-- : i++) {
        typedef typeof(avps) v;
        v::iterator o;

        // The size of committed AVPs can't be larger than @maxSize.
        size_t topIdx = dgram->offsetOf(top);
        if (maxSize)
            DIAASSERT(maxSize >= topIdx-cookie);

        // Find the current position (@from) of this AVP.
        byte *from = top;
        if (avpTbl[i].seen) {
            for (o = avps.begin(); ; ++o) {
                DIAASSERT(o != avps.end());
                if (o->first == i && o->second != 0)
                    break;
                from += o->second;
            }
        } else
            o = avps.end();

        // Handle replacements first.
        uint32_t n;
        size_t lreplacement;
        const void *replacement;
        if (avpTbl[i].avp == ORIGIN_HOST) {
            if ((replacement = myOriginHost) != NULL)
                lreplacement = strlen(replacement);
        } else if (avpTbl[i].avp == ORIGIN_REALM) {
            if ((replacement = myOriginRealm) != NULL)
                lreplacement = strlen(replacement);
        } else if (avpTbl[i].avp == RESULT_CODE) {
            n = htonl(resultCode);
            replacement = &n;
            lreplacement = sizeof(n);
        } else if (avpTbl[i].avp == ERROR_MESSAGE) {
            if ((replacement = errorMessage) != NULL)
                lreplacement = strlen(replacement);
        } else
            replacement = NULL;

        if (replacement) {
            size_t avpSize = MIN_AVP_SIZE + ALIGN4(lreplacement);

            if (maxSize && topIdx-cookie + avpSize > maxSize)
                // The resulting @dgram would be > @msgSize.  Since this
                // is a mandatory AVP, its lack is fatal.
                goto fatal;
            dgram = avpTbl[i].seen
                ? dgram->moveSpace(top, avpSize, from, o->second)
                : dgram->splice(top, 0, NULL, avpSize);
            if (!dgram)
                goto fatal;

            *dgramPtr = dgram;
            dia = fromDGram(dgram);
            msgSize += avpSize;
            if (avpTbl[i].seen)
                msgSize -= o->second;

            // Now there's @avpSize free space at @topIdx, which is
            // just enough to write our AVP there.
            top = dgram->at(writeAVP(dgram, topIdx,
                                     avpTbl[i].avp, FLAG_MANDATORY,
                                     lreplacement, replacement));

            if (o != avps.end()) {
                DIAASSERT(avpTbl[i].seen == 1);
                avpTbl[i].seen = o->second = 0;
                continue;
            }
        } // replace AVP

        // If it's an other known AVP move it to @top.  @dgram remains valid.
        if (avpTbl[i].seen) {
            if (maxSize && topIdx-cookie + o->second > maxSize) {
                // Can't add this AVP because the resulting @dgram
                // would be > @msgSize.
                dgram->mUsed = topIdx;
                msgSize = topIdx-cookie;
                break;
            }
            dgram->moveData(top, from, o->second);
            top += o->second;
            o->second = 0;
        } // move AVP
    } // for each @avpTbl

    // By this time all known AVPs (ie. those whose relative position is
    // specified in the standard) are in their correct place, and all other
    // AVPs are left behind them (in untouched order).

    // If @dia > @maxSize, drop some/all trailing AVPs.
    if (maxSize && msgSize > maxSize) {
        LOG("Message (%zu bytes) larger than %zu bytes, dropping AVPs",
            msgSize, maxSize);

        // @top points right after the last committed AVP.
        // @rem := number of bytes of committed AVPs
        rem = dia->offsetOf(top)-cookie;
        DIAASSERT(rem <= msgSize);
        DIAASSERT(maxSize >= rem);
        maxSize -= rem;

        // Consider AVPs from @top until we reach an AVP to be dropped.
        for (pos = top, rem = msgSize - rem; ;
             pos = const_cast<byte *>(nextPos)) {
             nextPos = dia->parseAVPHeader(pos, &rem, &avp, &avpFlags,
                                           &dataLen);
             // Since we know that the message is oversized, there must be
             // trailing AVP:s.  Also we shouldn't get an error because we
             // have parsed all AVP:s once before.
             DIAASSERT(nextPos != NULL);
             DIAASSERT(!(avpFlags & FLAG_PARSE_ERROR));

             // Similarly we know that skipAVPData() didn't fail.
             nextPos = dia->skipAVPData(nextPos, &rem, dataLen);
             DIAASSERT(nextPos != NULL);

             // Is this AVP above the @maxSize limit?
             if (maxSize < (size_t)(nextPos - pos)) {
                 // Yes, drop it and everything afterwards.
                 dgram->mUsed = dgram->offsetOf(pos);
                 break;
             } else
                 maxSize -= nextPos - pos;
        }
    }

    finishMessage(dgram, cookie);
    return true;

fatal:
    // If @dia is fatally broken, let's create a very dumbed down message
    // on our own.
    if (!(msgFlags & FLAG_REQUEST))
        myOriginHost = myOriginRealm = NULL;
    msgFlags &= ~FLAG_REQUEST;
    if (isError)
        msgFlags |=  FLAG_ERROR;
    else
        msgFlags &= ~FLAG_ERROR;
    return createSimpleMessage(dgramPtr, cmd, msgFlags, hbh, ete,
                               myOriginHost, myOriginRealm, resultCode);
}
// }}}

// Message parsing {{{
// Parse a DIAMETER message in the containing DGram starting at @pos
// or from the start of DGram if @pos is NULL.  *@msgRemPtr should be 0,
// except to indicate that only the first *@msgRemPtr bytes of the DGram
// are valid.
//
// If the method succeeds, it fills out the given fields (unless some
// of them is NULL), and returns a pointer pointing right after the
// header (ie. to the first AVP).  It is important (and mandatory)
// to preserve this return value and @msgRemPtr, because they are
// used by the parsing methods as state variables.
//
// Other possible return values are NULL if the byte sequence cannot
// be interpreted as a DIAMETER header, @pos (or &mData[0] if it was
// NULL) if it is possibly a valid header, but too few bytes are available
// (ie. more data is needed).
//
// NOTE: *@msgRemPtr is the remaining bytes of the DIAMETER message.
//       This routine doesn't guarantee that the complete message is
//       currently in the DGram.  Therefore it is possible that *@msgRemPtr
//       points _out_ of @mData.
const byte *Diameter::parseMessageHeader(const byte *pos, size_t *msgRemPtr,
                      unsigned *commandPtr, unsigned *flagsPtr,
                      unsigned *applicationIdPtr,
                      unsigned *hopByHopPtr, unsigned *endToEndPtr) const {
    // DIAMETER message header consists of five 4-byte parts.
    DIAASSERT(msgRemPtr != NULL);

    // Start from the beginning of the buffer?
    if (!pos)
        // cppcheck-suppress uselessAssignmentPtrArg
        pos = mData;

    // Incomplete header?
    if (firstUnused() < pos + HEADER_SIZE)
        return pos;

    if (*pos != PROTOCOL_VERSION) {
        ERR("Unknown protocol version %u", *pos);
        return NULL;
    }

    *msgRemPtr = ntohl(*(uint32_t *)pos) & 0x00FFFFFF;
    if (*msgRemPtr < HEADER_SIZE) {
        ERR("Too short message (%zu bytes)", *msgRemPtr);
        return NULL;
    }
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    if (flagsPtr)
        *flagsPtr = *pos;
    if (commandPtr)
        *commandPtr = ntohl(*(uint32_t *)pos) & 0x00FFFFFF;
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    if (applicationIdPtr)
        *applicationIdPtr = ntohl(*(uint32_t *)pos);
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    if (hopByHopPtr)
        *hopByHopPtr = ntohl(*(uint32_t *)pos);
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    if (endToEndPtr)
        *endToEndPtr = ntohl(*(uint32_t *)pos);
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    // We have successfully parsed the header.
    return pos;
}

// Parse the AVP header starting at @pos, which should be the return
// value of one of the other parsing methods, just like @msgRemPtr.
// It parses the AVP code, flags and the length of the AVP's data,
// and returns a pointer right after this header (ie. the at the data).
// You should either parse the data (which method to use depends on
// the AVP code and it's up to you), or skip it with skipAVPData().
//
// By default it is NOT verified that the AVP payload (*@dataLenPtr)
// fits in the DGram, but it can be turned on with @checkDataLen.
//
// If the end of message has reached (ie. no more AVP:s to parse),
// NULL is returned.  Otherwise if an error is detected (ie. the
// message is invalid in some way) NULL returned too and *flagsPtr
// is set to FLAG_PARSE_ERROR.
const byte *Diameter::parseAVPHeader(const byte *pos, size_t *msgRemPtr,
                             unsigned *avpCodePtr, unsigned *flagsPtr,
                             size_t *dataLenPtr, bool checkDataLen) const {
    size_t avpLen;
    unsigned plus;

    DIAASSERT(pos && msgRemPtr && avpCodePtr && flagsPtr && dataLenPtr);

    *flagsPtr = 0;
    if (*msgRemPtr == 0) {
        if (pos != firstUnused()) {
            ERR("%zu unprocessed bytes at the end of message",
                firstUnused() - pos);
            goto out;
        }
        return NULL;
    }

    // Is the remaining space too small for an AVP header?
    if (!checkSpace(pos, *msgRemPtr, MIN_AVP_SIZE))
        goto out;

    *avpCodePtr = ntohl(*(uint32_t *)pos);
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    *flagsPtr = *pos;
    avpLen = ntohl(*(uint32_t *)pos) & 0x00FFFFFF;
    pos += sizeof(uint32_t);
    *msgRemPtr -= sizeof(uint32_t);

    plus = 0;
    if (*flagsPtr & FLAG_VENDOR) {
        // Check the space for the Vendor ID, but ignore it.
        plus = sizeof(uint32_t);
        if (!checkSpace(pos, *msgRemPtr, sizeof(uint32_t)))
            goto out;
        pos += sizeof(uint32_t);
        *msgRemPtr -= sizeof(uint32_t);
    }

    // Verify that @avpLen is not too small.
    if (avpLen < MIN_AVP_SIZE + plus) {
        ERR("AVP %u has invalid length (%zu)", *avpCodePtr, avpLen);
        goto out;
    }

    // Verify that @avpLen is not too large.
    *dataLenPtr = avpLen - (MIN_AVP_SIZE + plus);
    if (!checkDataLen) {
        if (*msgRemPtr < ALIGN4(*dataLenPtr)) {
            ERR("AVP %u is too large (%zu/%zu)", *avpCodePtr,
                *dataLenPtr, *msgRemPtr);
            goto out;
        }
    } else if (!checkSpace(pos, *msgRemPtr, ALIGN4(*dataLenPtr)))
        goto out;

    // Return a pointer to the AVP data.
    return pos;

out:
    *flagsPtr = FLAG_PARSE_ERROR;
    return NULL;
}

// Parse a 32-bit integer-valued AVP.  @pos, @msgRemPtr and @dataLen
// should be what parseAVPHeader() returned.  It returns a pointer
// to the next AVP suitable for parseAVPHeader()ing, or NULL if an
// error is detected.
const byte *Diameter::parseInt32(const byte *pos, size_t *msgRemPtr,
                                 size_t dataLen, uint32_t *valuep) const {
    DIAASSERT(pos && msgRemPtr && valuep);

    if (dataLen != sizeof(*valuep)) {
        ERR("Invalid AVP data length %zu", dataLen);
        return NULL;
    } else if (!checkSpace(pos, *msgRemPtr, sizeof(*valuep)))
        return NULL;

    *valuep = ntohl(*(uint32_t *)pos);
    pos += sizeof(*valuep);
    *msgRemPtr -= sizeof(*valuep);

    return pos;
}

// Like parseInt32(), except that it returns a dynamically allocated,
// NUL-terminated string.
const byte *Diameter::parseString(const byte *pos, size_t *msgRemPtr,
                                  size_t dataLen, char **strp) const {
    size_t roundedDataLen;

    DIAASSERT(pos && msgRemPtr && strp);

    roundedDataLen = ALIGN4(dataLen);
    if (!checkSpace(pos, *msgRemPtr, roundedDataLen))
        return NULL;

    if (!(*strp = static_cast<char *>(malloc(dataLen + 1)))) {
        ERR("malloc(%zu): %m", dataLen + 1);
        return NULL;
    }

    memcpy(*strp, pos, dataLen);
    (*strp)[dataLen] = '\0';

    pos += roundedDataLen;
    *msgRemPtr -= roundedDataLen;

    return pos;
}

// Skip the data of an AVP.  Use it to skip AVP:s you don't know how
// to process.  Calling conventions and semantics are the same as for
// other parsing methods.
const byte *Diameter::skipAVPData(const byte *pos, size_t *msgRemPtr,
                                  size_t dataLen) const {
    DIAASSERT(pos && msgRemPtr);

    dataLen = ALIGN4(dataLen);
    if (!checkSpace(pos, *msgRemPtr, dataLen))
        return NULL;

    pos += dataLen;
    *msgRemPtr -= dataLen;
    return pos;
}

// Checks whether @dgram (potentially) contains a DIAMETER message.
// If so, it returns a pointer right after it.  Otherwise if the
// data in the @dgram cannot possibly be a DIAMETER message (ie.
// its header is invalid) it returns NULL.  Finally, it returns
// a pointer to the start of the @dgram if it cannot determine yet
// what is the situation because more data is needed.
//
// This method is suitable for setting as a defragmenter for an IOTask.
const byte *Diameter::isMessageComplete(const DGram *dgram,
                                        size_t gapAt, size_t gapSize) {
    size_t msgrem;
    const byte *next;
    const struct Diameter *dia;

    msgrem = gapAt;
    dia = static_cast<const struct Diameter *>(dgram);
    if (!(next = dia->parseMessageHeader(NULL, &msgrem))
        || next == dgram->mData)
        return next;
    else if ((next - dgram->mData) + msgrem > dgram->mUsed)
        return dgram->mData;
    else
        return next + msgrem;
}
// }}}

// Message dumping {{{
// LOG() the raw details of the AVP at @pos in @dia.  *@remp is the number
// of bytes this AVP may occupy.
const byte *Diameter::dumpAVP(const Diameter *dia, const byte *pos,
                              size_t *remp, unsigned depth) {
    size_t datalen;
    unsigned avp, flags, indent;

    if (!(pos = dia->parseAVPHeader(pos, remp, &avp, &flags, &datalen)))
        // Parse error.
        return NULL;

    // Print the AVP header with @intent:ataion.
    indent = (depth + 1) * 2;
    LOG("%*savp: %u, flags: %.2x", indent, "", avp, flags);
    LOG("%*sdata size: %zu, remaining: %zu bytes of %zu", indent, "",
        datalen, *remp, dia->firstUnused() - pos);

    switch (avp) {
    case REQUESTED_SERVICE_UNIT:
    case SUBSCRIPTION_ID:
    case USED_SERVICE_UNIT:
    case MULTIPLE_SERVICES_CC:
    case USER_EQUIPMENT_INFO:
    case SERVICE_INFORMATION:
    case PS_INFORMATION:
    case SMS_INFORMATION:
        // Dump AVP groups recursively.
        *remp -= datalen;
        while (datalen > 0)
            if (!(pos = dia->dumpAVP(dia, pos, &datalen, depth+1)))
                return NULL;
        break;
    default: // One day we might parse and print integers, strings etc.
        if (!(pos = dia->skipAVPData(pos, remp, datalen)))
            return NULL;
        break;
    }

    // AVP has been parsed successfully.
    return pos;
}

// LOG() raw details of a @dia:meter message starting at @pos or at the
// beginning.  Useful to verify the correctness of a Diameter message.
void Diameter::dumpMessage(const Diameter *dia, const byte *pos) {
    size_t rem;
    const byte *from;
    unsigned cmd, flags, app, hbh, ete;

    from = pos;
    LOG("Max message size: %zu bytes.",
        from ? dia->firstUnused() - from : dia->mUsed);
    if (!(pos = dia->parseMessageHeader(from, &rem, &cmd, &flags,
                                        &app, &hbh, &ete)))
        return;
    if (pos == from) {
        ERR("Incomplete message.");
        return;
    }

    // We have a complete header at least.  Go through the AVPs and dump them.
    LOG("app: 0x%.2x, cmd: %u, flags: 0x%.2x", app, cmd, flags);
    LOG("h2h: 0x%.8x, e2e: 0x%.8x", hbh, ete);
    LOG("remaining: %zu bytes of %zu", rem, dia->firstUnused() - pos);
    while (rem > 0)
        if (!(pos = dia->dumpAVP(dia, pos, &rem)))
            // Parse error.
            return;
    LOG("End of message.");
    // Message format is correct.
}
// }}}
// }}}

// Private variables {{{
// State variable of our rand() implementation.  It's intentionally not
// in the TLS as random number generation needn't be thread-safe--we just
// want a non-trivial sequenece of numbers.
static int32_t MyRanda;

// Jump to @Quit when a SIGINT or SIGTERM is caught.
static jmp_buf Quit;

// @Verbosity is the flag changed with the -vq flags.
// -- level 0: don't print anything traffic-related
// -- level 1: print the sent and received messages
// -- level 2: print the sent and received bytes
// -- level 3: decode and dump the sent and received messages
static unsigned Verbosity = 1;

// The file descriptors to write all @Input and @Output DGram:s to.
static int Input = -1, Output = -1;

// @SessionIdCounter is the Session-Id of the last UDR or PNR sent.
// @LastSessionId is the Session-Id of the last message examined by
// msgFromPeer().
//
// If @StartOfMeasurement is not zero, it designates the time proc_stdin()
// commenced a measurement of transaction speed.  When @LastMessageSent,
// its time is recorded.  The measurement ends and its duration is displayed
// when @LastSessionId reaches @SessionIdCounter.
static pthread_mutex_t MeasurementLock;
static uint64_t SessionIdCounter, LastSessionId;
static struct timespec StartOfMeasurement, LastMessageSent;
// }}}

// Classless functions {{{
static void sigint(int) {
    // Wake up the main thread when SIGINT or SIGTERM is received.
	longjmp(Quit, 1);
}

// Return a random integer from [@min, @max].  @max-@min must be < UINT_MAX.
static unsigned rndint(unsigned min, unsigned max) {
    return min < max ? min + rand() % (max - min + 1) : min;
}

// Fill @str in with up to @max-1 randomly chosen alphabethical characters
// and a NUL terminator.  If @max is 0 @str is left alone.
static void mkRandomString(char *str, unsigned max, unsigned min = 0) {
    unsigned i, sstr;

    if (!max)
        return;

    sstr = rndint(min, max-1);
    for (i = 0; i < sstr; i++) {
        unsigned n;

        n = rand() % (('9'-'0'+1) + ('z'-'a'+1) + ('Z'-'A'+1));
        if (n <= '9'-'0') {
            // Digit
            str[i] = '0' + n;
            continue;
        }

        n -= '9'-'0' + 1;
        if (n <= 'z'-'a') {
            // Lower letter
            str[i] = 'a' + n;
            continue;
        }

        // Capital letter
        n -= 'z'-'a' + 1;
        str[i] = n + 'A';
    }

    str[i] = '\0';
}

// Open @fname and write a PCAP header.
static int open_pcap(char const *fname) {
    int hfd;
    pcap_hdr_t pcap;

    if (!fname || (fname[0] == '-' && !fname[1]))
        hfd = STDOUT_FILENO;
    else if ((hfd = open(fname, O_CREAT|O_TRUNC|O_WRONLY|O_APPEND,
                        0666)) < 0) {
        ERR("open_pcap(%s): %s", fname, strerror(errno));
        return -1;
    }

    memset(&pcap, 0, sizeof(pcap));
    pcap.magic_number   = PCAP_MAGIC;
    pcap.version_major  = PCAP_VERSION_MAJOR;
    pcap.version_minor  = PCAP_VERSION_MINOR;
    pcap.snaplen        = PCAP_MAX_SNAPLEN;
    pcap.network        = PCAP_DLT_RAW_IPV4;
    if (write(hfd, &pcap, sizeof(pcap)) < 0) {
        ERR("open_pcap(): %s", strerror(errno));
        close(hfd);
        return -1;
    }

	return hfd;
}

// Write a PCAP packet header, IP header, SCTP DATA header and @payload
// to @hfd.  We use SCTP, because Wireshark doesn't decode DIAMETER in
// UDP, and TCP looks more complicated than SCTP.
static void write_pcap(int hfd, unsigned sport, unsigned dport,
                       const void *payload, size_t spayload) {
    net_hdr_t pkt;
    unsigned checksum;
    struct timeval now;
    struct iovec iov[2];

    memset(&pkt, 0, sizeof(pkt));
    gettimeofday(&now, NULL);
    pkt.pcap.ts_sec   = now.tv_sec;
    pkt.pcap.ts_usec  = now.tv_usec;
    pkt.pcap.incl_len = sizeof(pkt.ip) + sizeof(pkt.sctp) + spayload;
    pkt.pcap.orig_len = pkt.pcap.incl_len;

    pkt.ip.version  = 4;
    pkt.ip.ihl      = sizeof(pkt.ip) / sizeof(uint32_t);
    pkt.ip.tot_len  = htons(pkt.pcap.incl_len);
    pkt.ip.ttl      = 16;
    pkt.ip.protocol = IPPROTO_SCTP;
    pkt.ip.saddr    = htonl(INADDR_LOOPBACK);
    pkt.ip.daddr    = htonl(INADDR_LOOPBACK);

#if __BYTE_ORDER != __LITTLE_ENDIAN
# warning "This code has only be tested on little-endian machines,"
# warning "and is likely to break on machines with other bytesex."
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

    pkt.sctp.common.src_port = htons(sport);
    pkt.sctp.common.dst_port = htons(dport);

    pkt.sctp.data.first_fragment = 1;
    pkt.sctp.data.final_fragment = 1;
    pkt.sctp.data.chunk_length = htons(sizeof(pkt.sctp.data) + spayload);
    pkt.sctp.data.payload_protocol_identifier = htonl(SCTP_PPID_DIAMETER);

    iov[0].iov_base = &pkt;
    iov[0].iov_len  = sizeof(pkt);
    iov[1].iov_base = const_cast<void *>(payload);
    iov[1].iov_len  = spayload;
    if (writev(hfd, iov, MEMBS_OF(iov)) < 0)
        ERR("write_pcap(): %s", strerror(errno));
}

// Send @dgram through @sfd and free it afterwards.
static void sendDGram(int sfd, unsigned sport, unsigned dport,
                      DGram *dgram, unsigned stream = 0,
                      bool freeDGram = true) {
    bool okay;

    if (!dgram)
        return;

    if (Verbosity > 2)
        Diameter::dumpMessage(Diameter::fromDGram(dgram));

    okay = true;
    if (sfd >= 0) {
        if (stream == 0) {
            static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

            // Ensure atomicity if transmitting through TCP, because another
            // thread may want to send something at the same time.  SCTP does
            // not have this problem.
            pthread_mutex_lock(&lock);
            if (write(sfd, dgram->mData, dgram->mUsed) < 0) {
                ERR("write: %s", strerror(errno));
                okay = false;
            }
            pthread_mutex_unlock(&lock);
        } else {
            struct sctp_sndrcvinfo sinfo;

            memset(&sinfo, 0, sizeof(sinfo));
            sinfo.sinfo_stream = stream;
            if (sctp_send(sfd, dgram->mData, dgram->mUsed, &sinfo, 0) < 0) {
                ERR("sctp_send(%u): %s", stream, strerror(errno));
                okay = false;
            }
        }
    }

    if (Output >= 0)
        write_pcap(Output, sport, dport, dgram->mData, dgram->mUsed);
    if (okay && Verbosity > 1)
        LOG("write() %lu", dgram->mUsed);

    if (freeDGram)
        free(dgram);
}

// Return a human-readable translation of @cmd.
static const char *translate(unsigned cmd, unsigned flags) {
    static char str[32];
    const char *cc;
    bool isRequest = !!(flags & Diameter::FLAG_REQUEST);
    bool isError   = !!(flags & Diameter::FLAG_ERROR);

    if (cmd == Diameter::CER)
        cc = "CE";
    else if (cmd == Diameter::DWR)
        cc = "DW";
    else if (cmd == Diameter::DPR)
        cc = "DP";
    else if (cmd == Diameter::UDR)
        cc = "UD";
    else if (cmd == Diameter::PNR)
        cc = "PN";
    else {
        snprintf(str, sizeof(str), "CC%u %s%s", cmd,
                 isRequest ? "request" : "response",
                 isError ? " (E)" : "");
        return str;
    }

    snprintf(str, sizeof(str), "%s%c%s",
             cc, isRequest ? 'R' : 'A', isError ? " (E)" : "");
    return str;
}

// Create a simple DIAMETER message with @cmd, Result-Code (unless
// it's a request), Origin-Host and Origin-Realm.
static DGram *createSimpleMessage(const ConnectionCtx *ctx,
                                  unsigned cmd, bool isRequest = true,
                                  unsigned hbh = 0, unsigned ete = 0,
                                  unsigned rc = 0) {
    DGram *dgram;
    unsigned flags;

    if (!hbh)
        hbh = ctx->hop_by_hop;
    if (!ete)
        ete = ctx->end_to_end;

    dgram = NULL;
    flags = isRequest ? Diameter::FLAG_REQUEST : 0;
    if (!Diameter::createSimpleMessage(&dgram, cmd, flags,
                                       hbh, ete,
                                       ctx->origin.host, ctx->origin.realm,
                                       rc))
        return NULL;

    if (Verbosity > 0)
        LOG("-> %s", translate(cmd, flags));
    return dgram;
}

// Add a random Int32 @avp to *@dgramPtr with @vendor as Vendor-ID.
// If it failes and the AVP was to be a part of a group, roll it back.
static bool addRandomInt32AVP(DGram **dgramPtr, unsigned avp,
                               unsigned vendor = 0, unsigned groupStart = 0,
                               bool mandatory = true) {
    if (Diameter::addInt32AVP(dgramPtr, avp, rand(), mandatory, vendor))
        return true;
    else if (groupStart)
        (*dgramPtr)->mUsed = groupStart;
    return false;
}

// Like addRandomInt32AVP(), except that a random string is added.
static bool addRandomStringAVP(DGram **dgramPtr, unsigned avp,
                               unsigned vendor = 0, unsigned groupStart = 0,
                               bool mandatory = true) {
    char str[64+1];

    mkRandomString(str, sizeof(str));
    if (Diameter::addStringAVP(dgramPtr, avp, str, mandatory, vendor))
        return true;
    else if (groupStart)
        (*dgramPtr)->mUsed = groupStart;
    return false;
}

// Add Vendor-Id = LBSDIA_SUPPORTED_VENDOR_ID to *@dgramPtr.
// This is used as the first AVP in a group.
static bool addVendorId(DGram **dgramPtr, unsigned groupStart) {
    if (!Diameter::addInt32AVP(dgramPtr, Diameter::VENDOR_ID,
                               LBSDIA_SUPPORTED_VENDOR_ID)) {
        (*dgramPtr)->mUsed = groupStart;
        return false;
    } else
        return true;
}

// Add Session-Id (<DiameterIdentity>;<high 32 bits>;<low 32 bits>)
// to *@dgramPtr @at the specified position (or at the end of the dgram).
// If @dgramPtr is NULL, just return the size of the AVP.
static size_t addSessionId(DGram **dgramPtr, const ConnectionCtx *ctx,
                           uint64_t sessionId, size_t at = 0) {
    char str[64];
    size_t size, was;

    if (!dgramPtr) {
        size = strlen(ctx->origin.host) + 1 + 8 + 1 + 8;
        return Diameter::MIN_AVP_SIZE + ALIGN4(size);
    }

    // 72 bytes
    size = snprintf(str, sizeof(str), "%s;%.8lx;%.8lx",
                    ctx->origin.host,
                    sessionId >> 32, sessionId & 0xFFFFFFFF);
    size = ALIGN4(size);
    size += Diameter::MIN_AVP_SIZE;

    if (at) {
        was = (*dgramPtr)->mUsed;
        (*dgramPtr)->mUsed = at;
    }

    if (!Diameter::addStringAVP(dgramPtr, Diameter::SESSION_ID, str))
        size = 0;

    if (at)
        (*dgramPtr)->mUsed = was;

    return size;
}

// Add Vendor-Specific-Applicaion-Id to *@dgramPtr.
static bool addVESA(DGram **dgramPtr) {
    size_t group;

    if (!Diameter::startAVPGroup(dgramPtr, Diameter::VENDOR_SPECIFIC_APP_ID,
                                 &group))                       // 8 bytes
        return false;
    if (!addVendorId(dgramPtr, group))                          // 12 bytes
        return false;
    if (!Diameter::addInt32AVP(dgramPtr, Diameter::AUTH_APPLICATION_ID,
                               Diameter::TGPP_SH))              // 12 bytes
        return false;
    Diameter::finishAVPGroup(*dgramPtr, group);

    // Total: 32 bytes.
    return true;
}

// Add a Random User-Identity { Public-Identity, MSISDN } to *@dgramPtr.
static bool addUserIdentity(DGram **dgramPtr) {
    size_t userIdentity;

    if (Diameter::startAVPGroup(dgramPtr, Diameter::USER_IDENTITY,
                                 &userIdentity, true,   // 12 bytes
                                 Diameter::VENDOR_3GPP)
        && addRandomStringAVP(dgramPtr, Diameter::PUBLIC_IDENTITY,
                              Diameter::VENDOR_3GPP)    // 44 bytes
        && addRandomStringAVP(dgramPtr, Diameter::MSISDN,
                              Diameter::VENDOR_3GPP)) {
        Diameter::finishAVPGroup(*dgramPtr, userIdentity);
        return true;
    } else
        return false;
}

// Add a randomly generated User-Data to *@dgramPtr.
static bool addUserData(DGram **dgramPtr,
                        size_t minUserData, size_t maxUserData,
                        const char *publicId, const char *msISDN = NULL) {
    size_t n;

    // How large User-Data to add?
    n = rndint(minUserData, maxUserData);

    // Generate @userData and add it to *@dgramPtr.
    if (msISDN) {
        const char prefix[] = "Dear %s (%s), your user data is: ";
        const size_t lprefix = strlen(prefix)
            -2+strlen(publicId) + -2+strlen(msISDN);
        char userData[lprefix + n + 1];

        sprintf(userData, prefix, publicId, msISDN);
        mkRandomString(&userData[lprefix], n+1, n+1);
        return Diameter::addStringAVP(dgramPtr, Diameter::USER_DATA, userData,
                                      true, Diameter::VENDOR_3GPP);
    } else {
        const char prefix[] = "Dear %s, your user data is: ";
        const size_t lprefix = strlen(prefix) + -2+strlen(publicId);
        char userData[lprefix + n + 1];

        sprintf(userData, prefix, publicId);
        mkRandomString(&userData[lprefix], n+1, n+1);
        return Diameter::addStringAVP(dgramPtr, Diameter::USER_DATA, userData,
                                      true, Diameter::VENDOR_3GPP);
    }
}

// Construct and send a CER on @task, and set up @ctx->cerTimer.
// Returns whether both duties have been completed successfully.
static DGram *mkCERorCEA(const ConnectionCtx *ctx, bool isRequest = true) {
    DGram *cer;

    // Construct CER.  300 bytes are thought to be enough for it.
    if (!(cer = DGram::alloc(300)))
        return NULL;
    if (!Diameter::startMessage(&cer, Diameter::CER,            // 20 bytes
                                isRequest ? Diameter::FLAG_REQUEST : 0,
                                0, ctx->hop_by_hop, ctx->end_to_end))
        goto out;
    if (!Diameter::addInt32AVP(&cer, Diameter::RESULT_CODE,     // 12 bytes
                               Diameter::RC_SUCCESS))
        goto out;
    if (!Diameter::addStringAVP(&cer, Diameter::ORIGIN_HOST,    // ~40 bytes
                                ctx->origin.host))
        goto out;
    if (!Diameter::addStringAVP(&cer, Diameter::ORIGIN_REALM,   // ~40 bytes
                                ctx->origin.realm))
        goto out;

    // Discover our addresses and add them to @cer as Host-IP-Address.
    if (ctx-> sfd < 0)
        /* NOP */;
    else if (ctx->is_sctp) {
        int n;
        struct sockaddr *saddrs;
        const struct sockaddr *saddr;

        if ((n = sctp_getladdrs(ctx->sfd, 0, &saddrs)) < 0) {
            ERR("sctp_getladdrs(): %s", strerror(errno));
            goto out;
        } else
            saddr = saddrs;
        for (; n > 0; n--) {
            DMXEndPoint addr;

            // Max 28 bytes.
            saddr = addr.fromSockaddr(saddr);
            if (!Diameter::addAddrAVP(&cer, Diameter::HOST_IP_ADDR,
                                      &addr.addr)) {
                sctp_freeladdrs(saddrs);
                goto out;
            }
        }
        sctp_freeladdrs(saddrs);
    } else {
        DMXEndPoint addr;
        socklen_t ssaddr;
        struct sockaddr_storage saddr;

        if (getsockname(ctx->sfd, (struct sockaddr *)&saddr, &ssaddr) < 0)
            ERR("getsockname(): %s", strerror(errno));
        addr.fromSockaddr((struct sockaddr *)&saddr);
        if (!Diameter::addAddrAVP(&cer, Diameter::HOST_IP_ADDR,
                                  &addr.addr))
            goto out;
    }

    if (!Diameter::addInt32AVP(&cer, Diameter::VENDOR_ID,       // 12 bytes
                               LBSDIA_VENDOR_ID))
        goto out;
    if (!Diameter::addStringAVP(&cer, Diameter::PRODUCT_NAME,   // 16 bytes
                                LBSDIA_PRODUCT_NAME, false))
        goto out;
    if (!Diameter::addInt32AVP(&cer, Diameter::SUPPORTED_VENDOR_ID,
                               LBSDIA_SUPPORTED_VENDOR_ID))     // 12 bytes
        goto out;
    if (!addVESA(&cer))                                         // 32 bytes
        goto out;
    if (!Diameter::addInt32AVP(&cer, Diameter::FIRMWARE_REVISION, // 12 bytes
                               LBSDIA_FIRMWARE_REVISION, false))
        goto out;

    // Total: ~252 bytes.
    Diameter::finishMessage(cer);

    if (Verbosity > 0)
        LOG("-> %s", isRequest ? "CER" : "CEA");
    return cer;

out:
    free(cer);
    return NULL;
}

// Add AVPs to *@dgramPtr common in UDR and PNR.
//  Session-Id:                     addSessionId()
//  Vendor-Specific-Application-Id: addVESA()
//  Auth-Session-State:             AUTH_STATE_MAINTAINED
//  Origin-Host:                    @ctx->origin.host
//  Origin-Realm:                   @ctx->origin.realm
//  Destination-Host:               @ctx->destination.host
//  Destination-Realm:              @ctx->destination.realm
static bool startUDRorPNR(DGram **dgramPtr, const ConnectionCtx *ctx,
                          unsigned cmd, unsigned hopByHop = 0,
                          uint64_t sessionId = 0) {
    if (!hopByHop)
        hopByHop = ctx->hop_by_hop;
    if (!Diameter::startMessage(dgramPtr, cmd, Diameter::FLAG_REQUEST,
                                Diameter::TGPP_SH,      // 20 bytes
                                hopByHop, ctx->end_to_end))
        return false;

    // Session-Id, VESA, ASS
    if (!addSessionId(dgramPtr, ctx, sessionId))        // 72 bytes
        return false;
    if (!addVESA(dgramPtr))                             // 32 bytes
        return false;
    if (!Diameter::addInt32AVP(dgramPtr, Diameter::AUTH_SESSION_STATE,
                               Diameter::AUTH_STATE_MAINTAINED))
        return false;                                   // 12 bytes

    // {Origin,Destination}-{Host,Realm}                // ~4*40 bytes
    if (!Diameter::addStringAVP(dgramPtr, Diameter::ORIGIN_HOST,
                                ctx->origin.host))
        return false;
    if (!Diameter::addStringAVP(dgramPtr, Diameter::ORIGIN_REALM,
                                ctx->origin.realm))
        return false;
    if (!Diameter::addStringAVP(dgramPtr, Diameter::DESTINATION_HOST,
                                ctx->destination.host))
        return false;
    if (!Diameter::addStringAVP(dgramPtr, Diameter::DESTINATION_REALM,
                                ctx->destination.realm))
        return false;

    // Total: 296 bytes.
    return true;
}

// Generate a random User-Data-Request.
//  User-Identity:
//      Public-Identity:            mkRandomString()
//      MSISDN:                     mkRandomString()
//  Data-Reference:                 REPOSITORY_DATA
static DGram *mkUDR(const ConnectionCtx *ctx, uint64_t sessionId) {
    DGram *udr;

    if (!(udr = DGram::alloc(512)))
        return NULL;
    if (!startUDRorPNR(&udr, ctx, Diameter::UDR, 0, sessionId)) // 296 bytes
        goto out;

    // User-Identity, Data-Reference
    if (!addUserIdentity(&udr))
        goto out;
    if (!Diameter::addInt32AVP(&udr, Diameter::DATA_REFERENCE,
                               Diameter::REPOSITORY_DATA,
                               true, Diameter::VENDOR_3GPP))
        goto out;                                       // 16 bytes

    // Total: 412 bytes.
    Diameter::finishMessage(udr);

    if (Verbosity > 0)
        LOG("-> UDR");
    return udr;

out:
    free(udr);
    return NULL;
}

// Generate a random Push-Notification-Request.
static DGram *mkPNR(const ConnectionCtx *ctx,
                    unsigned hopByHop, uint64_t sessionId) {
    DGram *pnr;
    char publicIdentity[32+1];

    if (!(pnr = DGram::alloc(412 + ctx->max_user_data)))
        return NULL;
    if (!startUDRorPNR(&pnr, ctx, Diameter::PNR, hopByHop, sessionId))
        goto out;                   // 296 bytes

    // Public-Identity              // 44 bytes
    mkRandomString(publicIdentity, sizeof(publicIdentity));
    if (!Diameter::addStringAVP(&pnr, Diameter::PUBLIC_IDENTITY,
                                publicIdentity, true,
                                Diameter::VENDOR_3GPP))
        goto out;
    // User-Data                    12 + 29 + 31 + @maxUserData bytes
    if (!addUserData(&pnr, ctx->min_user_data, ctx->max_user_data,
                     publicIdentity))
        goto out;

    // Total: 412 + @maxUserData.
    Diameter::finishMessage(pnr);

    if (Verbosity > 0)
        LOG("-> PNR");
    return pnr;

out:
    free(pnr);
    return NULL;
}

// Return a randomly generated User-Data response.
static DGram *mkUDA(const ConnectionCtx *ctx, const Diameter *dia,
                    const byte *udr, size_t rem) {
    DGram *reply;
    size_t datalen;
    unsigned avp, flags;
    bool sessionIdFound;
    char *publicId, *msISDN;

    // Retrieve Session-Id, Public-Identity and MSISDN from @dia.
    reply = NULL;
    sessionIdFound = false;
    publicId = msISDN = NULL;
    while ((udr = dia->parseAVPHeader(udr, &rem, &avp, &flags, &datalen))) {
       if (avp == Diameter::USER_IDENTITY) {
            size_t group;

            group = datalen;
            if (rem < group) {
                ERR("Invalid USER_IDENTITY data length");
                goto out;
            }
            rem -= group;

            // This is a group AVP.
            if (!(udr = dia->parseAVPHeader(udr, &group,
                                            &avp, &flags, &datalen)))
                goto out;
            if (!(udr = dia->parseString(udr, &group, datalen, &publicId)))
                goto out;
            if (!(udr = dia->parseAVPHeader(udr, &group,
                                            &avp, &flags, &datalen)))
                goto out;
            if (!(udr = dia->parseString(udr, &group, datalen, &msISDN)))
                goto out;

            if (sessionIdFound)
                break;
        } else {
            if (avp == Diameter::SESSION_ID) {
                sessionIdFound = true;
                if (publicId && msISDN)
                    break;
            }
            if (!(udr = dia->skipAVPData(udr, &rem, datalen)))
                goto out;
        }
    } // for each AVP

    // Create a response.
    if (!(reply = dia->dupe()))
        goto out;

    // Have we found every AVP we were looking for?
    if (!sessionIdFound || !publicId || !msISDN) {
        // Reply an error message.
        if (!dia->makeResponse(&reply, true,
                               Diameter::RC_MISSING_AVP,
                               ctx->origin.host, ctx->origin.realm))
            goto out;
    } else {
        if (!dia->makeResponse(&reply, false,
                               Diameter::RC_SUCCESS,
                               ctx->origin.host, ctx->origin.realm))
            goto out;
        if (!addUserData(&reply, ctx->min_user_data, ctx->max_user_data,
                         publicId, msISDN))
            goto out;
        Diameter::finishMessage(reply);
    }

    if (Verbosity > 0)
        LOG("-> UDA");
    free(publicId);
    free(msISDN);
    return reply;

out:
    free(publicId);
    free(msISDN);
    free(reply);
    return NULL;
}

// Returns whether we're waiting for the end of a measurement of
// the round-trip time of multiple transactions.  During this period
// the user can't send messages.
static bool measurementInProgress() {
    return StartOfMeasurement.tv_sec || StartOfMeasurement.tv_nsec;
}

// Return the time in seconds elapsed @since.  If @nowp is not NULL,
// the current CLOCK_MONOTONIC time is taken from there.
static double measurementTime(
                        const struct timespec *since = &StartOfMeasurement,
                        struct timespec *nowp = NULL) {
    double elapsed;
    struct timespec now;

    if (!nowp) {
        nowp = &now;
        clock_gettime(CLOCK_MONOTONIC, &now);
    }

    elapsed  = (nowp->tv_sec  - since->tv_sec);
    elapsed += (nowp->tv_nsec - since->tv_nsec) / 1000000000.0;

    return elapsed;
}

// Depending on @ctx->is_client, return either an UDR or a PNR.
static DGram *mkUDRorPNR(const ConnectionCtx *ctx, uint64_t sessionId) {
    unsigned hbh;

    if (ctx->is_client)
        return mkUDR(ctx, sessionId);

    // If we're talking to DiaLBS the high 16-bit of the Hop-by-Hop Id
    // will decide which client gets our message.
    hbh  = rndint(ctx->min_lga, ctx->max_lga) << 16;
    hbh |= ctx->hop_by_hop & 0xF;
    return mkPNR(ctx, hbh, sessionId);
}

// Compile a DIAMETER message with randomly chosen AVPs.
static DGram *mkRandom(const ConnectionCtx *ctx, uint64_t sessionId) {
    DGram *dgram;
    unsigned navps;

    if (!(dgram = DGram::alloc(512)))
        return NULL;
    if (!Diameter::startMessage(&dgram,
                                ctx->is_client
                                    ? Diameter::UDR
                                    : Diameter::PNR,
                                Diameter::FLAG_REQUEST,
                                Diameter::TGPP_SH,
                                ctx->hop_by_hop, ctx->end_to_end)) {
        free(dgram);
        return NULL;
    }

    if (measurementInProgress() && !addSessionId(&dgram, ctx, sessionId))
        return NULL;

    // The return values of the add*() functions are not checked
    // intentionally, because their failure doesn't block the whole
    // operation.
    for (navps = rand() % 25; navps > 0; navps--) {
        size_t group;

        switch (rand() % 20) {
		case 0:  /* AUTH_SESSION_STATE */
            Diameter::addInt32AVP(&dgram,
                                  Diameter::AUTH_SESSION_STATE,
                                  rand() % 2);
            break;
		case 1:  /* DATA_REFERENCE */
            Diameter::addInt32AVP(&dgram,
                                  Diameter::DATA_REFERENCE,
                                  Diameter::REPOSITORY_DATA,
                                  true, Diameter::VENDOR_3GPP);
            break;
		case 2:  /* DESTINATION_HOST */
            addRandomStringAVP(&dgram, Diameter::DESTINATION_HOST);
            break;
		case 3:  /* DESTINATION_REALM */
            addRandomStringAVP(&dgram, Diameter::DESTINATION_REALM);
            break;
		case 4:  /* ERROR_MESSAGE */
            addRandomStringAVP(&dgram, Diameter::ERROR_MESSAGE);
            break;
		case 5:  /* ERROR_REPORTING_HOST */
            addRandomStringAVP(&dgram, Diameter::ERROR_REPORTING_HOST);
            break;
		case 6:  /* EXPERIMENTAL_RESULT */
            if (Diameter::startAVPGroup(&dgram,
                                         Diameter::EXPERIMENTAL_RESULT,
                                         &group)
                && addVendorId(&dgram, group)
                && addRandomInt32AVP(&dgram,
                                     Diameter::EXPERIMENTAL_RESULT_CODE,
                                     0, group))
                Diameter::finishAVPGroup(dgram, group);
            break;
		case 7:  /* EXPIRY_TIME */
            addRandomInt32AVP(&dgram, Diameter::EXPIRY_TIME,
                              Diameter::VENDOR_3GPP, 0, false);
            break;
		case 8:  /* FAILED_AVP */
            if (Diameter::startAVPGroup(&dgram, Diameter::FAILED_AVP, &group)
                && addRandomStringAVP(&dgram,Diameter::PRODUCT_NAME,0,group))
                Diameter::finishAVPGroup(dgram, group);
            break;
		case 9:  /* ORIGIN_HOST */
            addRandomStringAVP(&dgram, Diameter::ORIGIN_HOST);
            break;
		case 10: /* ORIGIN_REALM */
            addRandomStringAVP(&dgram, Diameter::ORIGIN_REALM);
            break;
		case 11: /* ORIGIN_STATE_ID */
            addRandomInt32AVP(&dgram, Diameter::ORIGIN_STATE_ID);
            break;
		case 12: /* PROXY_INFO */
            if (Diameter::startAVPGroup(&dgram, Diameter::PROXY_INFO, &group)
                && addRandomStringAVP(&dgram, Diameter::PROXY_HOST, 0, group)
                && addRandomStringAVP(&dgram,Diameter::PROXY_STATE,0,group))
                Diameter::finishAVPGroup(dgram, group);
            break;
		case 13: /* RESULT_CODE */
            addRandomInt32AVP(&dgram, Diameter::RESULT_CODE);
            break;
		case 14: /* SEND_DATA_INDICATION */
            addRandomInt32AVP(&dgram, Diameter::SEND_DATA_INDICATION,
                              Diameter::VENDOR_3GPP, 0, false);
            break;
		case 15: /* SESSION_ID */
            addSessionId(&dgram, ctx, rndint(0, UINT_MAX-1));
            break;
		case 16: /* SUBS_REQ_TYPE */
            addRandomInt32AVP(&dgram, Diameter::SUBS_REQ_TYPE,
                              Diameter::VENDOR_3GPP);
            break;
		case 17: /* SUPPORTED_FEATURES */
            if (Diameter::startAVPGroup(&dgram, Diameter::SUPPORTED_FEATURES,
                                         &group, false, Diameter::VENDOR_3GPP)
                && addVendorId(&dgram, group)
                && addRandomInt32AVP(&dgram, Diameter::FEATURE_LIST_ID,
                                     Diameter::VENDOR_3GPP, group, false)
                && addRandomInt32AVP(&dgram, Diameter::FEATURE_LIST,
                                     Diameter::VENDOR_3GPP, group, false))
                Diameter::finishAVPGroup(dgram, group);
            break;
		case 18: /* USER_IDENTITY */
            addUserIdentity(&dgram);
            break;
		case 19: /* VENDOR_SPECIFIC_APP_ID */
            addVESA(&dgram);
            break;
        }
    }

    Diameter::finishMessage(dgram);
    if (Verbosity > 0)
        LOG("-> %s", ctx->is_client ? "UDR" : "PNR");
    return dgram;
}

// Just allocate a DGram and start a DIAMETER message.
static DGram *mkEmpty(const ConnectionCtx *ctx) {
    DGram *dgram;
    unsigned cmd, hbh;

    if (ctx->is_client) {
        cmd  = Diameter::UDR;
        hbh  = ctx->hop_by_hop;
    } else {
        cmd  = Diameter::PNR;
        hbh  = rndint(ctx->min_lga, ctx->max_lga) << 16;
        hbh |= ctx->hop_by_hop & 0xF;
    }

    if (!(dgram = DGram::alloc(512)))
        return NULL;
    if (!Diameter::startMessage(&dgram, cmd, Diameter::FLAG_REQUEST,
                                Diameter::TGPP_SH,
                                hbh, ctx->end_to_end)) {
        free(dgram);
        return NULL;
    }

    return dgram;
}

// If we're a client talking to DiaLBS the output stream will decide
// which server our message is meant for.
static void sendMessage(const ConnectionCtx *ctx, DGram *dgram,
                        bool freeDGram = true) {
    if (ctx->is_client)
        sendDGram(ctx->sfd, DIAMETER_CLIENT_PORT, DIAMETER_SERVER_PORT,
                  dgram, rndint(ctx->min_stream, ctx->max_stream),
                  freeDGram);
    else // server
        sendDGram(ctx->sfd, DIAMETER_SERVER_PORT, DIAMETER_CLIENT_PORT,
                  dgram, 0, freeDGram);
}

// Handle incoming DIAMETER requests and replies.
static bool msgFromPeer(ConnectionCtx *ctx, const DGram *dgram) {
    const Diameter *dia;
    const byte *ptr;
    size_t rem;
    unsigned cmd, flags, hbh, ete;

    // The caller has made sure that we have a complete DIAMETER message.
    rem = 0;
    dia = Diameter::fromDGram(dgram);
    ptr = dia->parseMessageHeader(NULL, &rem, &cmd, &flags, NULL, &hbh, &ete);
    DIAASSERT(ptr && dgram->begin() < ptr && ptr <= dgram->firstUnused());

    if (Verbosity > 0)
        LOG("<- %s", translate(cmd, flags));
    if (Verbosity > 2)
        Diameter::dumpMessage(dia);
    switch (cmd) {
    case Diameter::CER: { // {{{
        unsigned avp;
        char *str;
        size_t datalen;
        unsigned resultCode;
        bool gotOriginHost, gotOriginRealm;

        // Reply to CER.
        if (flags & Diameter::FLAG_REQUEST) {
            sendDGram(ctx->sfd, DIAMETER_PORTS(ctx), mkCERorCEA(ctx, false),
                      dgram->mStreamId);
            return true;
        }

        // Parse the AVP:s looking for Origin-Host, Origin-Realm (which
        // must be present and equal to what we expect) and Result-Code
        // (which should be 2xxx).
        resultCode = 0;
        gotOriginHost = gotOriginRealm = false;
        while ((ptr = dia->parseAVPHeader(ptr, &rem, &avp, &flags,
                                          &datalen))) {
            switch (avp) {
            case Diameter::RESULT_CODE:
                if (!(ptr = dia->parseInt32(ptr, &rem, datalen,
                                            &resultCode)))
                    return true;
                break;
            case Diameter::ORIGIN_HOST:
                if (!(ptr = dia->parseString(ptr, &rem, datalen, &str)))
                    return true;
                if (strcmp(str, ctx->destination.host)) {
                    ERR("Origin-Host mismatch (%s vs. %s)",
                        str, ctx->destination.host);
                    free(str);
                    return true;
                } else {
                    free(str);
                    gotOriginHost = true;
                }
                break;
            case Diameter::ORIGIN_REALM:
                if (!(ptr = dia->parseString(ptr, &rem, datalen, &str)))
                    return true;
                if (strcmp(str, ctx->destination.realm)) {
                    ERR("Origin-Realm mismatch (%s vs. %s)",
                        str, ctx->destination.realm);
                    free(str);
                    return true;
                } else {
                    free(str);
                    gotOriginRealm = true;
                }
                break;
            default:
                if (!(ptr = dia->skipAVPData(ptr, &rem, datalen)))
                    return true;
                break;
            }

            // Stop as soon as we've got we're looking for.
            if (resultCode && gotOriginHost && gotOriginRealm) {
                if (flags & Diameter::FLAG_ERROR)
                    break;
                if (resultCode / 1000 != 2)
                    break;
                return true;
            }
        } // parse CEA

        // Log what went wrong.
        ERR("Bogus CEA");
        if (!(flags & Diameter::FLAG_PARSE_ERROR)) {
            if (flags & Diameter::FLAG_ERROR)
                ERR("Error %u", resultCode);
            else if (resultCode)
                ERR("Result-Code %u", resultCode);
            if (!gotOriginHost)
                ERR("Origin-Host missing from CEA");
            if (!gotOriginRealm)
                ERR("Origin-Realm missing from CEA");
        }
        return true; // }}}
    } case Diameter::DPR: { // {{{
        unsigned avp;
        size_t datalen;
        bool rebooting;

        // Disconnect-Peer
        if (!(flags & Diameter::FLAG_REQUEST))
            return false;

        // Is Disconnect-Cause == REBOOTING?
        // Nevermind parse errors.
        rebooting = false;
        while ((ptr = dia->parseAVPHeader(ptr, &rem, &avp, &flags,
                                          &datalen))) {
            if (avp == Diameter::DISCONNECT_CAUSE) {
                unsigned code;

                if ((ptr = dia->parseInt32(ptr, &rem, datalen, &code)))
                    rebooting = (code == Diameter::REBOOTING);
                break;
            } else if (!(ptr = dia->skipAVPData(ptr, &rem, datalen)))
                break;
        }
        if (rebooting)
            LOG("Server is rebooting.");

        // Reply if we can, but no para if we can't.
        sendDGram(ctx->sfd, DIAMETER_PORTS(ctx),
                  createSimpleMessage(ctx, Diameter::DPR, false, hbh, ete),
                  dgram->mStreamId);
        ctx->is_eof = true;
        return false; // }}}
    } case Diameter::DWR: // {{{
        // Device-Watchdog
        if (flags & Diameter::FLAG_REQUEST)
            sendDGram(ctx->sfd, DIAMETER_PORTS(ctx),
                  createSimpleMessage(ctx, Diameter::DWR, false, hbh, ete),
                  dgram->mStreamId);
        return true; // }}}
    case Diameter::UDR: // {{{
        if (flags & Diameter::FLAG_REQUEST) {
            if (ctx->recv_delay)
                usleep(ctx->recv_delay);
            if (ctx->no_reply)
                return true;
            sendDGram(ctx->sfd, DIAMETER_PORTS(ctx),
                      mkUDA(ctx, dia, ptr, rem), dgram->mStreamId);
            return true;
        }
        break; // }}}
    case Diameter::PNR: // {{{
        if (flags & Diameter::FLAG_REQUEST) {
            if (ctx->recv_delay)
                usleep(ctx->recv_delay);
            if (ctx->no_reply)
                return true;

            DGram *reply = dia->dupe();;
            if (reply && dia->makeResponse(&reply, false,
                                           Diameter::RC_SUCCESS,
                                           ctx->origin.host,
                                           ctx->origin.realm)) {
                if (Verbosity > 0)
                    LOG("-> PNA");
                sendDGram(ctx->sfd, DIAMETER_PORTS(ctx), reply,
                          dgram->mStreamId);
            } else
                free(reply);
            return true;
        }
        break; // }}}
    default: // ?
        return true;
    } // switch @cmd

    // We've got either UDA or PNA.  If a measurement is in progress,
    // check their Session-Id and if we've reached @SessionIdCounter,
    // stop the measurement and print the time elapsed since
    // @StartOfMeasurement. {{{
    pthread_mutex_lock(&MeasurementLock);
    if (measurementInProgress()) {
        unsigned avp;
        size_t datalen;
        char *sessionId;
        unsigned hi, lo;

        // Get Session-Id out of @dia.  It should be the first AVP.
        sessionId = NULL;
        if ((ptr = dia->parseAVPHeader(ptr, &rem, &avp, &flags, &datalen))
            && !(flags & Diameter::FLAG_PARSE_ERROR)
            && avp == Diameter::SESSION_ID
            && (ptr = dia->parseString(ptr, &rem, datalen, &sessionId))
            && sscanf(sessionId, "%*[^;];%x;%x", &hi, &lo) == 2
            && (LastSessionId = (((uint64_t)hi << 32) | lo))
                >= SessionIdCounter) {
            // Stop the measurement.
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            LOG("Test took %.3fs (%.3fs since the last message sent).",
                measurementTime(&StartOfMeasurement, &now),
                measurementTime(&LastMessageSent, &now));
            StartOfMeasurement.tv_sec = StartOfMeasurement.tv_nsec = 0;
        }
        free(sessionId);
    }
    pthread_mutex_unlock(&MeasurementLock);
    // measurement in progress }}}

    return true;
}
// }}}

// Thread entry points
// Process the commands received on the standard input. {{{
static void *proc_stdin(void *arg) {
    ConnectionCtx *ctx = static_cast<ConnectionCtx *>(arg);
    char line[10240]; // It's so large because of the "hexa" command.

    // Quit the program on EOF.
    while (fgets(line, sizeof(line), stdin)) {
        float f;
        uint64_t sessionId;
        unsigned n, min, max;
        char *cmd, *opt, *p, *q;
        bool dont_measure, no_number;

        // First process commands without number prefix.
        if (line[0] == '#')
            // A comment.
            continue;
        else if (!strcmp(line, "help\n")) {
            LOG("verbosity, verbose, quiet, role,\n"
                "\\n, <number-of-messages>, rnd, hexa, file, ?, cancel,\n"
                "noreply, doreply, watchdog, send-delay, recv-delay,\n"
                "streams, lga, user-data");
            continue;
        } else if (!strcmp(line, "verbosity\n")) {
            LOG("Current verbosity level is %u.", Verbosity);
            continue;
        } else if (sscanf(line, "verbosity %u\n", &n) == 1) {
            Verbosity = n;
            LOG("Verbosity level changed to %u.", Verbosity);
            continue;
        } else if (!strcmp(line, "verbose\n")) {
            Verbosity++;
            LOG("Verbosity level changed to %u.", Verbosity);
            continue;
        } else if (!strcmp(line, "quiet\n")) {
            Verbosity--;
            LOG("Verbosity level changed to %u.", Verbosity);
            continue;
        } else if (!strcmp(line, "role\n")) {
            LOG("I'm a Diameter %s.", ctx->is_client ? "client" : "server");
            continue;
        } else if (!strcmp(line, "cancel\n")) {
            // Cancel an ongoing measurement.
            pthread_mutex_lock(&MeasurementLock);
            if (measurementInProgress()) {
                LOG("Cancelled, time elapsed: %.3fs.", measurementTime());
                StartOfMeasurement.tv_sec = StartOfMeasurement.tv_nsec = 0;
            } else
                LOG("No measurement in progress.");
            pthread_mutex_unlock(&MeasurementLock);
            continue;
        } else if (!strcmp(line, "noreply\n")) {
            ctx->no_reply = true;
            LOG("autoreply off");
            continue;
        } else if (!strcmp(line, "doreply\n")) {
            ctx->no_reply = false;
            LOG("autoreply on");
            continue;
        } else if (!strcmp(line, "watchdog\n")) {
            LOG("Watchdog period is %.3fs.", ctx->watchdog_timeout/1000000.0);
            continue;
        } else if (sscanf(line, "watchdog %f", &f) == 1) {
            ctx->watchdog_timeout = f * 1000000.0;
            LOG("set");
            continue;
        } else if (!strcmp(line, "streams\n")) {
            LOG("use streams %u..%u", ctx->min_stream, ctx->max_stream);
            continue;
        } else if (sscanf(line, "streams %u %u", &min, &max) == 2) {
            if (!ctx->is_sctp)
                ERR("can't set streams on non-SCTP connection");
            else if (min <= max) {
                ctx->min_stream = min;
                ctx->max_stream = max;
                LOG("set");
            } else
                ERR("%u > %u", min, max);
            continue;
        } else if (sscanf(line, "streams %u", &ctx->min_stream) == 1) {
            if (ctx->is_sctp) {
                ctx->max_stream = ctx->min_stream;
                LOG("set");
            } else
                ERR("can't set streams on non-SCTP connections");
            continue;
        } else if (!strcmp(line, "lga\n")) {
            LOG("lga %u..%u", ctx->min_lga, ctx->max_lga);
        } else if (sscanf(line, "lga %u %u", &min, &max) == 2) {
            if (min <= max) {
                ctx->min_lga = min;
                ctx->max_lga = max;
                LOG("set");
            } else
                ERR("%u > %u", min, max);
            continue;
        } else if (sscanf(line, "lga %u", &ctx->min_lga) == 1) {
            ctx->max_lga = ctx->min_lga;
            LOG("set");
            continue;
        } else if (!strcmp(line, "user-data\n")) {
            LOG("generate User-Data between %u..%u",
                ctx->min_user_data, ctx->max_user_data);
            continue;
        } else if (sscanf(line, "user-data %u %u", &min, &max) == 2) {
            // Adjust the minimal and maximal size of User-Data we'll send.
            if (max < min)
                ERR("%u > %u", min, max);
            else if (max - min >= UINT_MAX)
                ERR("max-user-data (%u) is too large", max);
            else {
                ctx->min_user_data = min;
                ctx->max_user_data = max;
                LOG("set");
            }
            continue;
        } else if (sscanf(line, "user-data %u", &ctx->min_user_data) == 1) {
            // Make the size of User-Data fixed.
            ctx->max_user_data = ctx->min_user_data;
            LOG("set");
            continue;
        } else if (!strcmp(line, "send-delay\n")) {
            LOG("Delay between requests is %u us.", ctx->send_delay);
            continue;
        } else if (sscanf(line, "send-delay %f", &f) == 1) {
            ctx->send_delay = f * 1000.0;
            LOG("set");
            continue;
        } else if (!strcmp(line, "recv-delay\n")) {
            LOG("Delay before replies is %u us.", ctx->recv_delay);
            continue;
        } else if (sscanf(line, "recv-delay %f", &f) == 1) {
            ctx->recv_delay = f * 1000.0;
            LOG("set");
            continue;
        } else if (!strcmp(line, "?\n")) {
            pthread_mutex_lock(&MeasurementLock);
            LOG("SessionId: %lu of %lu", LastSessionId, SessionIdCounter);
            pthread_mutex_unlock(&MeasurementLock);
            continue;
        }

        // The rest of the commands may take a [!][<number>] prefix,
        // possibly followed by whitespace before the @cmd.
        dont_measure = line[0] == '!';
        n = strtoul(&line[dont_measure], &cmd, 10);
        no_number = cmd == &line[dont_measure];
        cmd += strspn(cmd, " \t");

        // NUL-terminate @cmd.
        p = cmd + strcspn(cmd, " \t\n");
        if (*p) {
            *p++ = '\0';
            p += strspn(p, " \t\n");
        }

        if (dont_measure && no_number && !cmd[0]) {
            // A '!' alone is meaningless.
            ERR("Syntax error.");
            continue;
        } else if (no_number)
            // If there isn't a number prefix, send a single message.
            n = 1;

        pthread_mutex_lock(&MeasurementLock);
        if (measurementInProgress()) {
            pthread_mutex_unlock(&MeasurementLock);
            ERR("measurement in progress");
            continue;
        } else if (!no_number && n > 0 && !dont_measure)
            // A non-zero number starts a measurement unless @dont_measure.
            clock_gettime(CLOCK_MONOTONIC, &StartOfMeasurement);

        // Operate on a copy of @SessionIdCounter so we can exit the
        // critical session.
        sessionId = SessionIdCounter;
        SessionIdCounter += n;
        pthread_mutex_unlock(&MeasurementLock);

        if (!cmd[0]) {
            for (; n > 0; n--) {
                sendMessage(ctx, mkUDRorPNR(ctx, ++sessionId));
                if (ctx->send_delay && n > 1)
                    usleep(ctx->send_delay);
            }

            clock_gettime(CLOCK_MONOTONIC, &LastMessageSent);
            if (!no_number)
                LOG("Sent.");

            continue;
        } else if (!strcmp(cmd, "rnd")) {
            for (; n > 0; n--) {
                sendMessage(ctx, mkRandom(ctx, ++sessionId));
                if (ctx->send_delay && n > 1)
                    usleep(ctx->send_delay);
            }

            clock_gettime(CLOCK_MONOTONIC, &LastMessageSent);
            if (!no_number)
                LOG("Sent.");

            continue;
        }

        // The rest of the commands take an optional switch and
        // a mandatory argument. Is the optional switch present?
        opt = NULL;
        if (p[0] == '-' && p[1]) {
            // Skip over it and the following whitespace.
            opt = &p[1];
            p += strcspn(p += 2, " \t\n");
            if (*p) {
                *p++ = '\0';
                p += strspn(p, " \t\n");
            }
        }

        if (*p) {
            // Trim the trailing whitespace.
            q = p + strlen(p) - 1;
            while (*q == ' ' || *q == '\t' || *q == '\n')
                *q-- = '\0';
        } else if (!strcmp(cmd, "file")) {
            // File name is mandatory for the file command.
            ERR("%s: required argument missing", cmd);
            continue;
        }

        if (!strcmp(cmd, "file") || !strcmp(cmd, "hexa")) {
            int c;
            FILE *st;
            DGram *dgram;
            unsigned skip;
            bool isbin, add_header, replace_header;

            // Verify @opt, if it was specified.
            isbin = false;
            add_header = true;
            replace_header = false;
            if (!strcmp(cmd, "hexa")) {
                if (opt && !opt[1]) {
                    if (opt[0] == 'h') {
                        replace_header = true;
                        opt = NULL;
                    } else if (opt[0] == 'H') {
                        add_header = false;
                        opt = NULL;
                    }
                }
                st = NULL;
            } else { // "file" command
                if (opt) {
                    // Is @p binary?
                    if (opt[0] == 'b') {
                        isbin = true;
                        opt++;
                    }

                    // Get @add_header and @replace_header.
                    if (!opt[0])
                        // Nothing followed -b.
                        opt = NULL;
                    else if (opt[1])
                        /* We sure don't know this option. */;
                    else if (opt[0] == 'h') {
                        replace_header = true;
                        opt = NULL;
                    } else if (opt[0] == 'H') {
                        add_header = false;
                        opt = NULL;
                    }
                }

                if (!(st = fopen(p, "r"))) {
                    ERR("%s: %m", p);
                    continue;
                }
            }

            // @opt must be NULL if it was parsed successfully.
            if (opt) {
                ERR("%s: -%s: unknown option", cmd, opt);
                continue;
            }

            // Start @dgram:        @add_header     @replace_header
            // --: prefix header    T               F
            // -h: replace header   T               T
            // -H: keep header      F               F
            skip = replace_header || (!add_header && measurementInProgress())
                ? Diameter::HEADER_SIZE : 0;
            dgram = add_header ? mkEmpty(ctx) : DGram::alloc(512);
            if (!dgram)
                goto error;

            // If we've just added a diameter header, add a Session-Id too
            // if measurementInProgress().
            if (add_header && measurementInProgress()
                && !addSessionId(&dgram, ctx, ++sessionId))
                goto error;

            // Read the file or parse @p and add its contents to @dgram.
            if (!st || !isbin) {
                // Parse hex numbers from @st or @p at most two digits
                // at a time.
                for (;;) {
                    // Parse the number.
                    if (st) {
                        if (fscanf(st, " %2x", &c) <= 0)
                            break;
                    } else {
                        int i;

                        while (*p == '_')
                            *p++ = ' ';
                        if (sscanf(p, " %2x%n", &c, &i) <= 0)
                            break;
                        p += i;
                    }

                    // Append @c to @dgram.
                    if (!skip || !add_header) {
                        if (!dgram->freeSpace() && !dgram->expand(&dgram, 64))
                            goto error;
                        *dgram->firstUnused() = c;
                        dgram->mUsed++;
                    }

                    if (skip) {
                        skip--;
                        if (skip > 0)
                            continue;
                        if (add_header || !measurementInProgress())
                            continue;
                        // -H, we've just got through the diameter header,
                        // and measurementInProgress(), add Session-Id.
                        if (!addSessionId(&dgram, ctx, ++sessionId))
                            goto error;
                    }
                }

                // Have we reached the message body?
                if (skip > 0) {
                    ERR("%s doesn't contain a diameter header", p);
                    goto error;
                }

                // Verify that nothing remained in the input.
                if (st) {
                    while ((c = getc(st)) != EOF)
                        if (!isspace(c)) {
                            ERR("%s: junk in file (%c)", p, c);
                            goto error;
                        }
                } else {
                    for (; *p; p++)
                        if (!isspace(*p)) {
                            ERR("junk (%c)", *p);
                            goto error;
                        }
                }
            } else
            {   // @st && @isbin
                size_t need;
                struct stat sbuf;

                if (fstat(fileno(st), &sbuf) < 0) {
                    ERR("%s: %m", p);
                    goto error;
                }

                need = sbuf.st_size;
                if (replace_header) {
                    // Skip the diameter header.
                    if (sbuf.st_size < Diameter::HEADER_SIZE) {
                        ERR("%s doesn't contain a diameter header", p);
                        goto error;
                    } else if (fseek(st, Diameter::HEADER_SIZE,
                               SEEK_SET) < 0) {
                        ERR("%s: %m", p);
                        goto error;
                    } else
                        need -= Diameter::HEADER_SIZE;
                }

                // Make sure @dgram has at least @need free space.
                if (!dgram->ensure(&dgram, need))
                    goto error;
                if (fread(dgram->firstUnused(), need, 1, st) != 1) {
                    ERR("%s: %m", p);
                    goto error;
                } else
                    dgram->mUsed += need;
            }

            // Finalize @dgram if we touched it.
            if (dgram->mUsed % sizeof(uint32_t) != 0) {
                ERR("message body not padded");
                goto error;
            }
            if (add_header || measurementInProgress())
                Diameter::finishMessage(dgram);

            // Send the message(s).
            for (;;) {
                sendMessage(ctx, dgram, false);
                if (!--n)
                    break;
                // Update the Session-Id.
                addSessionId(&dgram, ctx, ++sessionId, Diameter::HEADER_SIZE);
                if (ctx->send_delay)
                    usleep(ctx->send_delay);
            }

            clock_gettime(CLOCK_MONOTONIC, &LastMessageSent);
            if (!no_number)
                LOG("Sent.");

error:      free(dgram);
            if (st)
                fclose(st);
        } else
            ERR("%s: unknown command", cmd);
    } // until EOF

    kill(getpid(), SIGINT);
    return NULL;
} // }}}

// Receive and respond to network messages. {{{
static void *proc_network(void *arg) {
    DGramTmplAry<char, 65536> dgram;
    ConnectionCtx *ctx = static_cast<ConnectionCtx *>(arg);

    for (;;) {
        int n;

        if (!dgram.freeSpace()) {
            ERR("Message too large");
            break;
        }

        if (ctx->is_sctp) {
            struct sctp_sndrcvinfo sinfo;

            memset(&sinfo, 0, sizeof(sinfo));
            n = sctp_recvmsg(ctx->sfd, dgram.firstUnused(), dgram.freeSpace(),
                             NULL, NULL, &sinfo, NULL);
            dgram.mStreamId = sinfo.sinfo_stream;
        } else {
            n = read(ctx->sfd, dgram.firstUnused(), dgram.freeSpace());
            dgram.mStreamId = 0;
        }

        if (n < 0) {
            if (errno == EINTR)
                continue;
            ERR("read(%d): %s", ctx->sfd, strerror(errno));
            break;
        } else if (!n) {
            if (Verbosity > 0)
            {
                LOG("<- EOF");
                ctx->is_eof = true;
            }
            break;
        } else
            dgram.mUsed += n;

        // Process @dgram.  It may contain 0 or multiple Diameter messages.
        for (;;) {
            size_t rest;
            const byte *next;

            if (!(next = Diameter::isMessageComplete(&dgram))) {
                ERR("Invalid message received.");
                goto out;
            } else if (next == dgram.begin())
                // Incomplete message.
                break;

            // The Diameter parser expects a single message in @dgram.
            rest = dgram.firstUnused() - next;
            dgram.mUsed = next - dgram.begin();

            if (Input >= 0)
                write_pcap(Input, DIAMETER_PORTS(ctx),
                           dgram.mData, dgram.mUsed);
            if (!msgFromPeer(ctx, &dgram))
                goto out;

            if (!(dgram.mUsed = rest))
                break;
            memmove(dgram.begin(), next, dgram.mUsed);
        }
    }

out:
    kill(getpid(), SIGINT);
    return NULL;
} // }}}

// Send DWRs periodically. {{{
static void *__attribute__((noreturn)) watchdog(void *arg) {
    const ConnectionCtx *ctx = static_cast<const ConnectionCtx *>(arg);

    for (;;) {
        if (ctx->watchdog_timeout) {
            usleep(ctx->watchdog_timeout);
            sendDGram(ctx->sfd, DIAMETER_PORTS(ctx),
                      createSimpleMessage(ctx, Diameter::DWR));
        } else
            // Watchdog has been disabled, but poll it periodically,
            // because it might be re-enabled.
            sleep(5);
    }
} // }}}

// The main function {{{
int main(int argc, char *const argv[])
{
    static const struct option longopts[] = { // {{{
        { "help",           no_argument,        NULL, 'Z' },
        { "verbose",        no_argument,        NULL, 'v' },
        { "quiet",          no_argument,        NULL, 'q' },
        { "client",         no_argument,        NULL, 'c' },
        { "server",         no_argument,        NULL, 's' },
        { "no-stdin",       no_argument,        NULL, 'S' },
        { "no-reply",       no_argument,        NULL, 'D' },
        { "no-net",         no_argument,        NULL, 'N' },
        { "lbsdia",         no_argument,        NULL, 'L' },
        { "write-input",    required_argument,  NULL, 'O' },
        { "write-output",   required_argument,  NULL, 'o' },
        { "hop-by-hop",     required_argument,  NULL, 'i' },
        { "end-to-end",     required_argument,  NULL, 'I' },
        { "origin-host",    required_argument,  NULL, 'h' },
        { "origin-realm",   required_argument,  NULL, 'r' },
        { "dest-host",      required_argument,  NULL, 'H' },
        { "dest-realm",     required_argument,  NULL, 'R' },
        { "watchdog",       required_argument,  NULL, 't' },
        { "send-delay",     required_argument,  NULL, 'u' },
        { "recv-delay",     required_argument,  NULL, 'U' },
        { "min-stream",     required_argument,  NULL, 'a' },
        { "max-stream",     required_argument,  NULL, 'A' },
        { "min-hbh",        required_argument,  NULL, 'b' },
        { "max-hbh",        required_argument,  NULL, 'B' },
        { "min-payload",    required_argument,  NULL, 'm' },
        { "max-payload",    required_argument,  NULL, 'M' },
        { 0 },
    }; // }}}
    int optchar;
    sigset_t sigs;
    ConnectionCtx ctx;
    bool nocmd, nonet, lbsdia;
    struct sockaddr_storage saddr;
    pthread_t command_thread, watchdog_thread;

    // Preset defaults.  The value of @max_user_data has been chosen so
    // that UDR and PNR generation takes about the same time.
    memset(&ctx, 0, sizeof(ctx));
    ctx.sfd = STDOUT_FILENO;
    ctx.hop_by_hop = 3333;
    ctx.end_to_end = 4444;
    ctx.max_user_data = 352;
    ctx.watchdog_timeout = 5 * 1000000;

    // Parse the command line. {{{
    nocmd = nonet = lbsdia = false;
    while ((optchar = getopt_long(argc, argv,
                            "vqcsSDNLO:o:w:i:I:h:r:H:R:t:u:U:a:A:b:B:m:M:",
                            longopts, NULL)) != EOF) {
        switch (optchar) {
        case 'Z':
            puts("usage: radiator -vq -cs -SDN -L "
                 "-O <input-pcap> -o <output-pcap> -w <fname> "
                 "-i <hop-by-hop> -I <end-to-end> "
                 "-h <origin-host> -r <origin-realm> "
                 "-H <destination-host> -R <desination-realm> "
                 "-t <watchdog-timeout> -u <send-delay> -U <recv-delay> "
                 "-aA <min/max-streams> -bB <min/max-hbh> "
                 "-mM <min/max-user-data>");
            return 0;
        case 'v':
            Verbosity++;
            break;
        case 'q':
            Verbosity--;
            break;

        case 'S':
            nocmd = true;
            break;
        case 'D':
            ctx.no_reply = true;
            break;
        case'N':
            nonet = true;
            break;

        case 'c':
            ctx.is_client = true;
            break;
        case 's':
            ctx.is_client = false;
            break;
        case 'L':
            lbsdia = true;
            break;

        case 'O':
            if ((Input = open_pcap(optarg)) < 0)
                return 1;
            break;
        case 'o':
            if ((Output = open_pcap(optarg)) < 0)
                return 1;
            break;
        case 'w':
            if ((Input = open_pcap(optarg)) < 0)
                return 1;
            Output = Input;
            break;

        case 'i':
            ctx.hop_by_hop = strtoul(optarg, NULL, 0);
            break;
        case 'I':
            ctx.end_to_end = strtoul(optarg, NULL, 0);
            break;

        case 'h':
            ctx.origin.host = optarg;
            break;
        case 'r':
            ctx.origin.realm = optarg;
            break;
        case 'H':
            ctx.destination.host = optarg;
            break;
        case 'R':
            ctx.destination.realm = optarg;
            break;

        case 't':
            ctx.watchdog_timeout = atof(optarg) * 1000000.0;
            break;
        case 'u':
            ctx.send_delay = atof(optarg) * 1000.0;
            break;
        case 'U':
            ctx.recv_delay = atof(optarg) * 1000.0;
            break;

        case 'a':
            ctx.min_stream = ctx.max_stream = atoi(optarg);
            break;
        case 'A':
            ctx.max_stream = atoi(optarg);
            break;
        case 'b':
            ctx.min_lga = ctx.max_lga = atoi(optarg);
            break;
        case 'B':
            ctx.max_lga = atoi(optarg);
            break;
        case 'm':
            ctx.min_user_data = ctx.max_user_data = atoi(optarg);
            break;
        case 'M':
            ctx.max_user_data = atoi(optarg);
            break;

        default:
            exit(1);
        } // process option
    } // for all options

    // We don't expect positional arguments.
    if (argv[optind]) {
        ERR("too many arguments");
        return 1;
    } // }}}

    // Postprocess the parameters. {{{
    if (nonet)
        ctx.sfd = -1;
    if (nocmd && nonet) {
        ERR("--no-stdin and --no-net: what am I supposed to do?");
        return 1;
    }

    // Verify that ctx.min_* >= ctx.max_*.
    if (ctx.min_stream < ctx.max_stream) {
        ERR("min-stream (%u) > max-stream (%u)",
            ctx.min_stream, ctx.max_stream);
        return 1;
    } else if (ctx.min_lga > ctx.max_lga) {
        ERR("min-lga (%u) > max-lga (%u)", ctx.min_lga, ctx.max_lga);
        return 1;
    } else if (ctx.min_user_data > ctx.max_user_data) {
        ERR("min-user-data (%u) > max-user-data (%u)",
            ctx.min_user_data, ctx.max_user_data);
        return 1;
    } else if (ctx.max_user_data - ctx.min_user_data >= UINT_MAX) {
        // rndint() can't cope with such a large range.
        ERR("max-user-data (%u) is too large", ctx.max_user_data);
        return 1;
    }

    // Set the low 16 bits of our Hop-by-Hop Id to our local port number.
    socklen_t slen = sizeof(saddr);
    if (!nonet && !getsockname(ctx.sfd, (struct sockaddr *)&saddr, &slen)) {
        int proto;

        if (ctx.hop_by_hop <= 0xffff) {
            if (saddr.ss_family == AF_INET) {
                const struct sockaddr_in *saddr4;

                saddr4 = DMXEndPoint::toCS4(&saddr);
                ctx.hop_by_hop |= ntohs(saddr4->sin_port) << 16;
            } else if (saddr.ss_family == AF_INET6) {
                const struct sockaddr_in6 *saddr6;

                saddr6 = DMXEndPoint::toCS6(&saddr);
                ctx.hop_by_hop |= ntohs(saddr6->sin6_port) << 16;
            }
        }

        // Is @ctx.sfd SCTP?
        slen = sizeof(proto);
        if (getsockopt(ctx.sfd, SOL_SOCKET, SO_PROTOCOL, &proto, &slen) < 0) {
            ERR("getsockopt(%d, SO_PROTOCOL): %s", ctx.sfd, strerror(errno));
            return 1;
        } else {
            ctx.is_sctp = proto == IPPROTO_SCTP;
            if (!ctx.is_sctp && ctx.min_stream > 0) {
                ERR("can only use more than one streams on SCTP");
                return 1;
            }
        }

        // If we're a client, the server is DiaLBS and we're running
        // over SCTP, subscribe to sctp_sndrcvinfo data.  This is
        // necessary in order to see on which stream we got a request.
        if (ctx.is_client && ctx.is_sctp && lbsdia) {
            struct sctp_event_subscribe events;

            memset(&events, 0, sizeof(events));
            events.sctp_data_io_event = 1;
            if (setsockopt(ctx.sfd, SOL_SCTP, SCTP_EVENTS,
                           &events, sizeof(events)) < 0) {
                ERR("setsockopt(%d, SCTP_EVENTS): %s",
                    ctx.sfd, strerror(errno));
                return 1;
            }
        } // we're a client and DiaLBS is the server
    } // ctx.sfd is a socket

    // ctx.origin.* depends on whether we're the client.
    // ctx.destination.* depends on whether we're talking to LBSDiaCore,
    // or otherwise if we're the client.
    if (!ctx.origin.host)
        ctx.origin.host = ctx.is_client
            ? "radiator-client-host"
            : "radiator-server-host";
    if (!ctx.origin.realm)
        ctx.origin.realm = ctx.is_client
            ? "radiator-client-realm"
            : "radiator-server-realm";
    if (!ctx.destination.host) {
        if (lbsdia)
            ctx.destination.host = "lbsdia-host";
        else if (ctx.is_client)
            ctx.destination.host = "radiator-server-host";
        else
            ctx.destination.host = "radiator-client-host";
    }
    if (!ctx.destination.realm) {
        if (lbsdia)
            ctx.destination.realm = "lbsdia-realm";
        else if (ctx.is_client)
            ctx.destination.realm = "radiator-server-realm";
        else
            ctx.destination.realm = "radiator-client-realm";
    }

    // Makes no sense to send DWRs to DiaLBS, as it would forward them
    // to the server, which is supervised by the load balancer anyway.
    if (ctx.is_client && lbsdia && ctx.watchdog_timeout) {
        ctx.watchdog_timeout = 0;
        LOG("Watchdog disabled.");
    }
    // }}}

    srand(time(NULL));
    pthread_mutex_init(&MeasurementLock, NULL);

    // Say hello to the server unless we're talking to DiaLBS,
    // which doesn't expect it.
    if (ctx.is_client && !lbsdia)
        sendDGram(ctx.sfd, DIAMETER_PORTS(&ctx), mkCERorCEA(&ctx));

    // sigint() will make us quit.
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    signal(SIGINT, sigint);
    signal(SIGTERM, sigint);

    // Start the threads (if we need to) and enter the main loop.
    if (nonet) {
        DIAASSERT(!nocmd);
        if (!setjmp(Quit)) {
            pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
            proc_stdin(&ctx);
        }
    } else if (nocmd && !ctx.watchdog_timeout) {
        DIAASSERT(!nonet);
        if (!setjmp(Quit)) {
            pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
            proc_network(&ctx);
        }
    } else { // !@nonet && (!@nocmd || @ctx.watchdog_timeout)
        // We'll have at least one another thread.
        if (!nocmd)
            pthread_create(&command_thread, NULL, proc_stdin,  &ctx);
        if (ctx.watchdog_timeout)
            pthread_create(&watchdog_thread, NULL, watchdog, &ctx);
        if (!setjmp(Quit)) {
            pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
            proc_network(&ctx);
        }
    }

    // Say proper good-bye to the peer and to the user.  Stupid DiaLBS
    // forwards *all* requests to the server, so sending DPR would cause
    // the server to disconnect.
    if (!ctx.is_eof && (!ctx.is_client || !lbsdia))
        sendDGram(ctx.sfd, DIAMETER_PORTS(&ctx),
                  createSimpleMessage(&ctx, Diameter::DPR));
    LOG("Bye-bye");
	return 0;
} // }}}

/* vim: set textwidth=75 expandtab tabstop=4 shiftwidth=4: */
/* vim: set cinoptions=:0,g0,(0: */
/* vim: set foldmethod=marker foldmarker={{{,}}}: */
/* End of radiator.cc */
