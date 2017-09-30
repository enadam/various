#!/usr/bin/perl -w
#
# TODO support AVP_GROUPED (half-assed)
# TODO support hexa string input ([H] AVP flag)
# TODO binary output

use strict;

# Constants
# Diameter message header size and AVP header sizes
use constant MSG_HEADER_SIZE	=> 1+3 + 1+3 + 4*3;	# 20 bytes
use constant AVP_HEADER_SIZE	=> 4 + 1+3;		# 8 bytes

# Message header flags
use constant FLAG_REQUEST	=> 1 << 7;
use constant FLAG_PROXY		=> 1 << 6;
use constant FLAG_ERROR		=> 1 << 5;
use constant FLAG_REXMIT	=> 1 << 4;

# AVP header flags
use constant FLAG_VND_SPECIFIC	=> 1 << 7;
use constant FLAG_MANDATORY	=> 1 << 6;
use constant FLAG_PROTECTED	=> 1 << 5;

# Basic AVP value types
use constant AVP_STRING		=> 0;
use constant AVP_UNSIGNED	=> 1;
use constant AVP_ADDRESS	=> 2;
use constant AVP_GROUPED	=> 3;

# Fields in %AVPs
use constant AVP_CODE           => 0;
use constant AVP_TYPE           => 1;

# Fields in the list of lists print_avps() get.
use constant AVP_REF		=> 0;
use constant AVP_FLAGS		=> 1;
use constant AVP_VENDOR		=> 2;
use constant AVP_DATA		=> 3;
use constant AVP_SIZE		=> 4;

# For AVP_ADDRESS values
use constant ADDR_IPV4		=> 1;
use constant ADDR_IPV6		=> 2;

# Enterprise codes (Vendor-Is et al)
use constant VENDOR_NSN		=> 28458;
use constant VENDOR_3GPP	=> 10415;

# Result-Code:s
use constant RC_SUCCESS		=> 2001;
use constant RC_UNABLE_TO_DELIVER => 3002;
use constant RC_UNABLE_TO_COMPLY=> 5012;
use constant RC_MISSING_AVP	=> 5005;

# Disconnect-Cause values
use constant CAUSE_REBOOTING	=> 0;
use constant CAUSE_FUCK_YOU	=> 2; # DO_NOT_WANT_TO_TALK_TO_YOU

# Auth-Session-State values
use constant STATE_MAINTAINED	  => 0;
use constant STATE_NOT_MAINTAINED => 1;

use constant BASE_PROTOCOL	=> 0;
my %APPLICATIONS =
(
	BASE			=> BASE_PROTOCOL,
	BASE_ACCT		=> 3, # Same as Rf.
	SH			=> 16777217,
	RF			=> 3,
	RO			=> 4,
	RELAY			=> 0xFFFFFFFF,
);

my %COMMANDS =
(
	capabilities_exchange	=> 257,
	device_watchdog		=> 280,
	disconnect_peer		=> 262,
	user_data		=> 306,
	push_notification	=> 309,
);

my %AVPs =
(
	session_id		=> [ 263, AVP_STRING	],
	host_ip_address		=> [ 257, AVP_ADDRESS	],
	inband_security_id	=> [ 299, AVP_UNSIGNED	], # NO_INBAND_SECURITY

	origin_host		=> [ 264, AVP_STRING	],
	origin_realm		=> [ 296, AVP_STRING	],
	destination_host	=> [ 293, AVP_STRING	],
	destination_realm	=> [ 283, AVP_STRING	],

	vendor_id		=> [ 266, AVP_UNSIGNED	], # VENDOR_*
	supported_vendor_id	=> [ 265, AVP_UNSIGNED	], # VENDOR_*
	auth_application_id	=> [ 258, AVP_UNSIGNED	], # %APPLICATIONS
	acct_application_id	=> [ 259, AVP_UNSIGNED	], # %APPLICATIONS
	vendor_specific_application_id =>
				   [ 260, AVP_GROUPED	],
				#  { Vendor-Id, Auth/Acct-App-Id }

	result_code		=> [ 268, AVP_UNSIGNED	], # %RESULT_CODES
	disconnect_cause	=> [ 273, AVP_UNSIGNED	], # CAUSE_*

	experimental_result_code=> [ 298, AVP_UNSIGNED	],
	experimental_result	=> [ 297, AVP_GROUPED	],
				#  { Vendor-Id, Experimental-Result-Code }

	failed_avp		=> [ 279, AVP_GROUPED	], # Any AVPs
	error_message		=> [ 281, AVP_STRING	],
	error_reporting_host	=> [ 294, AVP_STRING	],

	proxy_host		=> [ 280, AVP_STRING	],
	proxy_state		=> [  33, AVP_STRING	],
	proxy_info		=> [ 284, AVP_GROUPED	],
				#  { Proxy-Host, Proxy-State }

	product_name		=> [ 269, AVP_STRING	],
	firmware_revision	=> [ 267, AVP_UNSIGNED	],

	origin_state_id		=> [ 278, AVP_UNSIGNED	],
	auth_session_state	=> [ 277, AVP_UNSIGNED	], # STATE_*

	# 3GPP AVPs
	public_identity		=> [ 601, AVP_STRING	],
	msisdn			=> [ 701, AVP_STRING	],
	user_identity		=> [ 700, AVP_GROUPED	],
				#  { Public-Identity, MSISDN }

	data_reference		=> [ 703, AVP_UNSIGNED	],
	user_data		=> [ 702, AVP_STRING	],
	ue_id			=> [ 411, AVP_STRING	],

	feature_list_id		=> [ 629, AVP_UNSIGNED	],
	feature_list		=> [ 630, AVP_UNSIGNED	],
	supported_features	=> [ 628, AVP_GROUPED	],
	#  { Vendor-Id, Feature-List-Id, Feature-List }

	send_data_indication	=> [ 710, AVP_UNSIGNED	], # Enumerated
	subs_req_type		=> [ 705, AVP_UNSIGNED	],
	expiry_time		=> [ 709, AVP_UNSIGNED	], # Time
);

my $RE_DECIMAL		= qr/^\d+$/;
my $RE_HEX_OCT_BIN	= qr/^(?:0[xX][0-9a-fA-F]+|0[0-7]+|0b[01]+)$/;

# Program code
# Private functions
sub parse_unsigned
{
	my $str = shift;

	if ($str =~ $RE_HEX_OCT_BIN)
	{	# oct() can convert all of them.
		return oct($str);
	} elsif ($str =~ $RE_DECIMAL)
	{
		return $str;
	} else
	{
		return undef;
	}
}

sub parse_ipv6
{
	my $addr = shift;
	my ($tail, @head, @tail);

	for (;;)
	{
		if (@head + @tail >= 8)
		{	# Too many parts.
			return ();
		} elsif ($addr =~ s/^:://)
		{	# Double double colons?
			return () if $tail;

			# Further parts are to be added to @tail.
			$tail = 1;

			# Have we consumed all of the string?
			last if length($addr) == 0;
		} elsif ($addr =~ s/^://)
		{	# Invalid leading colon?
			(@head || @tail)
				or return ()
		} elsif (@head || @tail)
		{	# Too many digits in a part.
			return ();
		}

		if ($addr !~ s/^([a-fA-F0-9]{1,4})//)
		{	# Not a 1..4 digit hexadecimal number.
			return ();
		} elsif (!$tail)
		{
			push(@head, $1);
		} else
		{
			push(@tail, $1);
		}

		last if !length($addr);
	}

	if ($tail)
	{	# Pad the middle with zeroes.
		return (@head, (0) x (8 - @head - @tail), @tail);
	} elsif (@head == 8)
	{
		return @head;
	} else
	{
		return ();
	}
}

sub parse_ip_address
{
	my $addr = shift;
	my @addr;

	if (@addr = ($addr =~ /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/))
	{
		return sprintf('%.4x' . ('%.2x' x @addr), ADDR_IPV4, @addr)
			unless grep($_ > 255, @addr);
	} elsif (@addr = parse_ipv6($addr))
	{
		return sprintf('%.4x%s', ADDR_IPV6,
			join('', map('0' x padding4(length($_)) . $_,
				@addr)));
	}

	return undef;
}

sub lookup_symbol
{
	my ($avp, $val) = (shift, uc(shift));

	$val =~ tr/ -/__/;
	if ($avp eq "vendor_id" || $avp eq "supported_vendor_id")
	{
		if ($val eq "NSN")
		{
			return VENDOR_NSN;
		} elsif ($val eq "3GPP")
		{
			return VENDOR_3GPP;
		}
	} elsif ($avp eq "result_code")
	{
		if ($val eq "SUCCESS")
		{
			return RC_SUCCESS;
		} elsif ($val eq "UNABLE_TO_DELIVER")
		{
			return RC_UNABLE_TO_DELIVER;
		} elsif ($val eq "UNABLE_TO_COMPLY")
		{
			return RC_UNABLE_TO_COMPLY;
		} elsif ($val eq "MISSING_AVP")
		{
			return RC_MISSING_AVP;
		}
	} elsif ($avp eq "disconnect_cause")
	{
		if ($val eq "REBOOTING")
		{
			return CAUSE_REBOOTING;
		} elsif ($val eq "FUCK_YOU")
		{
			return CAUSE_FUCK_YOU;
		}
	} elsif ($avp eq "auth_application_id"
		|| $avp eq "acct_application_id")
	{
		return $APPLICATIONS{$val};
	} elsif ($avp eq "auth_session_state")
	{
		if ($val eq "MAINTAINED")
		{
			return STATE_MAINTAINED;
		} elsif ($val eq "NOT_MAINTAINED")
		{
			return STATE_NOT_MAINTAINED;
		}
	}

	return undef;
}

sub padding4($)
{
	my $len = shift;
	my $rem = $len % 4;
	return $rem ? (4-$rem) : 0;
}

sub calculate_avp_sizes
{
	my $avps = shift;
	my ($total, @groups);
	my ($idx, $avp);

	$total = 0;
	while (($idx, $avp) = each(@$avps))
	{
		my ($type, $this, $padding);

		if (!defined $avp)
		{	# End of group
			pop(@groups);
			next;
		} elsif (!ref $avp)
		{
			$this = length($avp);
			die if $this % 2;
			$this /= 2;
			die if $this % 4;

			$total += $this;
			die "wtf" if $total % 4;
			$$avps[$_][AVP_SIZE] += $this
				foreach @groups;
			next;
		}

		$type = $$avp[AVP_REF][AVP_TYPE];
		$$avp[AVP_SIZE] = AVP_HEADER_SIZE;
		$$avp[AVP_SIZE] += 4
			if defined $$avp[AVP_VENDOR];
		$padding = 0;

		if ($type == AVP_STRING)
		{
			$this = length($$avp[AVP_DATA]);
			$padding = padding4($this);
		} elsif ($type == AVP_UNSIGNED)
		{
			$this = 4;
		} elsif ($type == AVP_ADDRESS)
		{
			$this = length($$avp[AVP_DATA]);
			die "wtf" if $this % 2;
			$this /= 2;
			$padding = padding4($this);
		} elsif ($type == AVP_GROUPED)
		{
			push(@groups, $idx);
			$total += $$avp[AVP_SIZE];
			next;
		} else
		{
			die "wtf";
		}

		die "wtf" if !defined $this;
		$$avp[AVP_SIZE] += $this;
		$this = $$avp[AVP_SIZE] + $padding;

		$total += $this;
		die "wtf" if $total % 4;

		$$avps[$_][AVP_SIZE] += $this
			foreach @groups;
	}

	@groups == 0
		or die "unclosed group";

	return $total;
}

sub print_avps
{
	my $avps = shift;

	foreach (@$avps)
	{
		my ($code, $type);

		next if !defined $_;
		if (!ref $_)
		{
			printf('%s', $_);
			next;
		}

		($code, $type) = @{$$_[AVP_REF]};
		printf('%.8x%.2x%.6x', $code, $$_[AVP_FLAGS], $$_[AVP_SIZE]);
		printf('%.8x', $$_[AVP_VENDOR])
			if defined $$_[AVP_VENDOR];

		if ($type == AVP_STRING)
		{
			printf('%.2x', $_)
				for map(ord($_), split(//, $$_[AVP_DATA]));
			printf('00' x padding4(length($$_[AVP_DATA])));
		} elsif ($type == AVP_UNSIGNED)
		{
			printf('%.8x', $$_[AVP_DATA]);
		} elsif ($type == AVP_ADDRESS)
		{
			printf('%s', $$_[AVP_DATA]);
			printf('00' x padding4(length($$_[AVP_DATA]) / 2));
		} elsif ($type == AVP_GROUPED)
		{
			print_avps($$_[AVP_DATA]);
		} else
		{
			die "wtf";
		}
	}
}

# Main starts here
my $line;

$\ = "\n";
$SIG{'__DIE__'} = sub
{
	my $msg = shift;
	$msg =~ s/ at \Q$0\E line (\d+), <> line \d+\.\n$//o;
	print STDERR "$ARGV:$.: $msg (line $1)";
	exit 1;
};

# The main loop
for (;;)
{
	my ($protocol_version, $dia_application, $command, $ucmd);
	my ($is_request, $header_flags);
	my ($hop_by_hop, $end_to_end);
	my @avps;

	if (!defined $line)
	{
		if (eof(ARGV))
		{
			close(ARGV);
			last if eof();
		}

		if (!defined ($line = <>))
		{	# Should not happend because the either eof(ARGV)
			# returned false (in which case something has to
			# be there to read) or this wasn't the last ARGV
			# file and the next readline() will return the
			# first line of the next ARG file.
			die "wtf";
		} elsif ($line =~ /^\s*(?:#|$)/)
		{	# Empty $line or all-whitespace or some whitespace
			# and a comment.
			undef $line;
			next;
		} elsif ($line =~ s/^\s+//)
		{	# Message is started by the command, which must be
			# at the beginning of the $line.
			chomp($line);
			die "Diameter command missing";
		}
	}

	# Verify that the command line is in the correct format.  Also
	# delete possible comment and preceeding whitespace till the end
	# of $line.
	chomp($line);
	$line =~ s/\s*:\s*(?:#.*)?$//
		or die "invalid Diameter command line";
	$ucmd = $line;

	# Normalize $command.
	$command = lc($ucmd);
	$command =~ tr/-/_/;

	# Determine whether this is a request or a reply or we don't know yet,
	# must be specified in the Flags header.  $^R contsins the return
	# value of the $(?{ }) expressions.
	$is_request = $command =~ s/_(?:request(?{1})
			|(?:answer|response|reply)(?{0}))$//x
		? $^R : undef;

	# Determine the Diameter command.
	defined ($command = $COMMANDS{$command})
		or die "$ucmd: unknown command";

	# Set up defaults.
	$protocol_version = 1;
	$dia_application = BASE_PROTOCOL;
	$header_flags = $hop_by_hop = $end_to_end = 0;

	# Parse further headers and the AVPs.
	for (;;)
	{
		my $avp;
		my ($ukey, $key);
		my ($val, $is_string);
		my ($uvnd, $vendor_id);
		my ($uflags, $set_avp_flags, $avp_flags);

		if (eof(ARGV))
		{
			undef $line;
			last;
		} elsif (!defined ($line = <>))
		{
			die "wtf";
		} elsif ($line =~ /^\s*#/)
		{	# Skip lines containing only a comment.
			next;
		} elsif ($line =~ /^\s*$/)
		{	# Empty lines terminate the message.
			undef $line;
			last;
		} elsif($line !~ s/^\s+//)
		{	# Non-indented lines start a new message.
			# Note that $line is kept and will be processed above.
			last;
		} elsif ($line =~ /^---+$/)
		{	# End of group
			push(@avps, undef);
			next;
		} else
		{
			chomp($line);
		}

		# Split $line into $ukey [$avp_flags] vendor=$vendor_id: $val.
		($ukey, $uflags, $uvnd, $val) = ($line =~ /
			^([A-Za-z0-9_-]+) \s*	# $hkey
			(?:\[([!A-Z]*)\] \s*)?	# AVP flags
			(?i:\ vendor \s*=\s*([a-z0-9_-]+) \s*)?
						# Vendor-Id
			: \s*			# separator
			(.+)?$/x);		# $val
		defined $ukey
			or die "$ucmd: parse error";

		# Parse AVP flags.  By default AVPs are mandatory.
		$uflags //= "";
		$set_avp_flags = 1;
		$avp_flags = FLAG_MANDATORY;
		while (length($uflags) > 0)
		{
			my $flag;

			if ($uflags =~ s/^!//)
			{
				$set_avp_flags = 0;
				length($uflags) > 0
					or die "$ucmd:$ukey: trailing '!' "
						. "in the AVP flags";
				next;
			} elsif ($uflags =~ s/^M//)
			{
				$flag = FLAG_MANDATORY;
			} elsif ($uflags =~ s/^P//)
			{
				$flag = FLAG_PROTECTED;
			} else
			{
				die "$ucmd:$ukey: $uflags: unknown AVP flag";
			}

			$set_avp_flags
				? ($avp_flags |=  $flag)
				: ($avp_flags &= ~$flag);
		} # until $uflags is consumed entirely

		# Find out the $vendor_id if vendor=xxx was specified.
		if (defined $uvnd)
		{
			defined ($vendor_id = parse_unsigned($uvnd))
				or defined ($vendor_id = lookup_symbol(
					"vendor_id", $uvnd))
				or die "$ucmd:$ukey: $uvnd: unknown vendor";
			$avp_flags |= FLAG_VND_SPECIFIC;
		}

		# Preprocess $val and determine whether it's an AVP_STRING.
		if (!defined $val)
		{	# NOP, it must be an AVP_GROUPED
		} elsif ($val =~ s/^"//)
		{	# $val is double-quoted, so it must be a string.
			$is_string = 2;

			# Is there junk (neither whitespace nor command)
			# after the closing '"'?  (the initial '"' has
			# been chopped already).  If so, delete it.
			$val =~ s/(?<!\\)"\s*(?:#.*)?$//
				or die "$ucmd:$ukey: couldn't parse '\"$val'";
		} else
		{	# $val is possibly an AVP_STRING.  First normalize it.
			# Delete everything after the first non-escaped '#'.
			$val =~ s/(?<!\\)#.*$//;

			# Condense strings of non-escaped spaces to a single
			# space.  Also remove leading and triling whitespace.
			$val =~ s/(^)?(?<!\\)\s?\s+($)?/
				defined $1 || defined $2 ? "" : " "/ge;

			# Are there special characters in $val?
			if ($val =~ /[^a-zA-Z0-9 _.-]/)
			{	# Yeah,, so $val must be an unquoted string.
				$is_string = 1;

				# Reject unescaped '"' marks.
				$val !~ /(?<!\\)"/
					or die "$ucmd:$ukey: unescaped '\"'";

				# Reject unescaped trailing '\'es.
				$val !~ /(?<!\\) (\\\\)* \\$/x
					or die "$ucmd:$ukey: "
						. "training backslash";
			}
		}

		# If $val $is_string, replace escaped # "\#", "\ ", '\"'
		# and "\\" characters, and complain if we encounter an
		# unknown escape sequence.
		$val =~ s!(\\(?:([# rn"\\])|.))!
				$2 // die "unknown escape sequence $1"!ge
			if $is_string;

		# $key := normalized $ukey
		$key = lc($ukey);
		$key =~ tr/-/_/;

		# Parse $key.
		if (!defined $val || (!length($val) && !$is_string))
		{	# Verify that $key is a grouped AVP.
			my $avp = $AVPs{$key};
			defined $avp
				or die "$ucmd:$ukey: unknown AVP";
			$$avp[AVP_TYPE] == AVP_GROUPED
				or die "$ucmd:$ukey: missing value";
			push(@avps, [ $avp, $avp_flags, $vendor_id ]);
		} elsif ($key eq "version")
		{	# Version: 1
			$protocol_version = $is_string
				? undef : parse_unsigned($val);
			die "$ucmd:$ukey: \"$val\": integer expected"
				if !defined $protocol_version;
		} elsif ($key eq "flags")
		{	# Flags: error reply
			# Reject double-quoted strings.
			die "$ucmd:$key: Diameter header flags expected"
				if $is_string && $is_string > 1;

			# Flags can be separated by spaces or commas.
			# Empty flags are allowed and ignored.
			foreach (split(/(?:\s*,\s*|\s+)/, $val))
			{
				my $flag = lc($_);
				if ($flag eq "request")
				{	# FLAG_REQUEST will be set later.
					die "$ucmd is not a request"
						if defined $is_request
							&& !$is_request;
					$is_request = 1;
				} elsif (grep($flag eq $_,
					qw(answer response reply)))
				{	# $header_flags won't be set.
					die "$ucmd is a request"
						if $is_request;
					$is_request = 0;
				} elsif ($flag eq "error")
				{
					$header_flags |= FLAG_ERROR;
				} elsif ($flag eq "proxiable"
					|| $flag eq "proxyable")
				{
					$header_flags |= FLAG_PROXY;
				} elsif ($flag eq "retransmitted")
				{
					$header_flags |= FLAG_REXMIT;
				} else
				{
					die "$ucmd: $_: unknown flag";
				}
			} # parse $header_flags
		} elsif ($key eq "application")
		{	# Application: Sh
			die "$ucmd:$ukey: Diameter application expected"
				if $is_string;
			defined ($dia_application = $APPLICATIONS{uc($val)})
				or die "$ucmd:$ukey: $val: "
					. "unknown Diameter application";
		} elsif ($key eq "hop_by_hop")
		{	# Hop-by-Hop: 0x1234
			$hop_by_hop = $is_string
				? undef : parse_unsigned($val);
			die "$ucmd:$ukey: \"$val\": integer expected"
				if !defined $hop_by_hop;
		} elsif ($key eq "end_to_end")
		{	# End-to-End: 0x4321
			$end_to_end = $is_string
				? undef : parse_unsigned($val);
			die "$ucmd:$ukey: \"$val\": integer expected"
				if !defined $end_to_end;
		} elsif ($key eq "hexadecimal")
		{
			$val =~ s/[_ ]+//g;
			$val =~ /^[0-9a-fA-F]+$/
				or die "$ucmd:$ukey: not a hex string";
			length($val) % 2 == 0
				or $val =~ s/(.)$/0$1/;
			push(@avps, $val);
		} elsif (!defined ($avp = $AVPs{$key}))
		{
			die "$ucmd:$ukey: unknown AVP";
		} elsif ($$avp[AVP_TYPE] == AVP_STRING)
		{	# Use $val as is, because it's stringified.
			$val //= "";
			push(@avps, [ $avp, $avp_flags, $vendor_id, $val ]);
		} elsif ($$avp[AVP_TYPE] == AVP_UNSIGNED)
		{	# Try to convert $val to an integer.
			my $ival;

			if ($is_string)
			{
				die "$ucmd:$ukey: $val: integer expected";
			} elsif (!defined ($ival = parse_unsigned($val))
				&& !defined ($ival=lookup_symbol($key, $val)))
			{
				die "$ucmd:$ukey: $val: unknown symbol";
			}

			push(@avps, [ $avp, $avp_flags, $vendor_id, $ival ]);
		} elsif ($$avp[AVP_TYPE] == AVP_ADDRESS)
		{	# Parse an IPv4 or IPv6 address.
			my $addr;

			$addr = parse_ip_address($val)
				unless $is_string && $is_string > 1;
			die "$ucmd:$ukey: \"$val\": IPv4/6 address expected"
				if !defined $addr;

			push(@avps, [ $avp, $avp_flags, $vendor_id, $addr ]);
		} elsif ($$avp[AVP_TYPE] == AVP_GROUPED)
		{	# AVP_GROUPED has been dealt with,
			die "$ucmd:$ukey: garbage at the end of line";
		} # switch $key
	} # parse headers and AVPs

	# Write the message.
	defined $is_request
		or die "$ucmd: request or response?";
	$header_flags |= FLAG_REQUEST
		if $is_request;

	printf('%.2x%.6x%.2x%.6x%.8x%.8x%.8x',
		$protocol_version,
		MSG_HEADER_SIZE + calculate_avp_sizes(\@avps),
		$header_flags, $command, $dia_application,
		$hop_by_hop, $end_to_end);
	print_avps(\@avps);
	print '';
}
