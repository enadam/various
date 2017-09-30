#!/usr/bin/perl -w
#
# tapcap.pl -- display a tcpdump in a human-readable format
#
# The primary use of this program is to replace IP addresses in a tcpdump
# with symbolic names (whose DNS name may not be available), to switch the
# "<sender> -> <receiver>" order to "<receiver> <- <sender>" if it makes
# sense, and to shorten the packet description part (currently DIAMETER
# commands are understood and abbreviated).
#
# Synopsis:
#   tapcap.pl [-ids] [<capture-files>]... [-- [<tshark-options>]...]
#   tapcap.pl -S
#
# Options:
#   -i		By default we skip packets whose sender or receiver is not
#		known (see below).  One `-i' makes us include packets whose
#		sender or receiver is not known, but the other party is.
#		Double `-i' makes us include all packets.
#   -s		Only display SCTP messages.
#   -d		Only display DIAMETER messages.
#   -sd		Only display SCTP and DIAMETER messages.
#   -S		Print a skeleton shell script with some standard symbols.
#
# Symbolic names of IP addresses can be specified via environment variables:
#   tapcap_lofasz="1.2.3.4"
#   tapcap_joska="4.3.2.1"
# causes us to replace 1.2.3.4 with "lofasz" and 4.3.2.1 with "joska".
#
# If no <capture-files> are specified, the _tshark-decoded_ form is read
# from the standard input (ie. you have to feed tapcap.pl with the output
# of tshark).
#

# Modules
use strict;
use Getopt::Long;

# Constants
# Replace LHS with RHS in the packet description if it's a DIAMETER message.
my @DIACOMMANDS =
(
	[ qr/^Capabilities-ExchangeRequest\(\d+\).*/,	'CER' ],
	[ qr/^Capabilities-ExchangeAnswer\(\d+\).*/,	'CEA' ],
	[ qr/^Disconnect-PeerRequest\(\d+\).*/,		'DPR' ],
	[ qr/^Disconnect-PeerAnswer\(\d+\).*/,		'DPA' ],
	[ qr/^Device-WatchdogRequest\(\d+\).*/,		'DWR' ],
	[ qr/^Device-WatchdogAnswer\(\d+\).*/,		'DWA' ],
);

# Private variables
my ($Opt_ignore_unknown, $Opt_SCTP_only, $Opt_DIAMETER_only);
my %Machines;

# Private functions
sub process
{
	my $fh = shift;
	my ($lseq, $ltime, $llhs, $lrhs, @lines);

	# Read each line until EOF and remember them in @lines.
	# We can't show the @lines as we go because we need to know
	# the width of the fields so that we can produce aligned
	# output.
	while (defined ($_ = readline($fh)))
	{
		my ($seq, $time, $from, $to, $msg);
		my ($m1, $m2, $lhs, $rhs, $arrow);

		($seq, $time, $from, undef, $to, $msg) = split(' ', $_, 6);
		next if !defined $msg;

		# Look up the symbolic names of $from and $to.
		# Skip this line if one or both are unknown and
		# !$Opt_ignore_unknown.
		$m1 = $Machines{$from};
		$m2 = $Machines{$to};
		if (defined $m1 && defined $m2)
		{
			$from = $m1;
			$to   = $m2;
		} elsif (!defined $m1 && !defined $m2)
		{
			next if $Opt_ignore_unknown < 2;
		} elsif (!defined $m1)
		{
			next if !$Opt_ignore_unknown;
			$to = $m2;
		} else
		{	# !defined #m2
			next if !$Opt_ignore_unknown;
			$from = $m1;
		}

		# Process the packet description part of the line.
		chop($msg);
		if ($msg =~ /^SCTP /)
		{
			next if !$Opt_SCTP_only && $Opt_DIAMETER_only;
		} elsif ($msg =~ s/^DIAMETER //)
		{
			next if $Opt_SCTP_only && !$Opt_DIAMETER_only;

			my $issack = $msg =~ s/^SACK //;
			if ($msg =~ s/^cmd=//)
			{
				foreach (@DIACOMMANDS)
				{
					last if $msg =~ s/$$_[0]/$$_[1]/;
				}
			}

			$msg .= ' ack' if $issack;
		} elsif ($Opt_SCTP_only || $Opt_DIAMETER_only)
		{
			next;
		}

		# Change the direction of the arrow so that it points to
		# the "local" address.
		if ($from !~ /_local$/ && $to =~ /_local$/)
		{
			($lhs, $rhs) = ($to, $from);
			$arrow = '<-';
		} else
		{
			($lhs, $rhs) = ($from, $to);
			$arrow = '->';
		}

		# Remember the length of the widest fields.
		defined $lseq && $lseq > length($seq)
			or $lseq = length($seq);
		defined $ltime && $ltime > length($time)
			or $ltime = length($time);
		defined $llhs && $llhs > length($lhs)
			or $llhs = length($lhs);
		defined $lrhs && $lrhs > length($rhs)
			or $lrhs = length($rhs);

		push(@lines, [ $seq, $time, $lhs, $arrow, $rhs, $msg ]);
	}

	# Print @lines.
	foreach (@lines)
	{
		printf('%*s %*s %*s %s %*s %s' . "\n",
			$lseq,	$$_[0],
			$ltime, $$_[1],
			$llhs,	$$_[2],
			$$_[3],
			$lrhs,	$$_[4],
			$$_[5]);
	}
}

# Main starts here
my @tshark;

# Parse the command line.
for (my $i = 0; $i < @ARGV; $i++)
{
	if ($ARGV[$i] eq '--')
	{
		@tshark = splice(@ARGV, $i);
		shift(@tshark);
		last;
	}
}

$Opt_ignore_unknown = 0;
Getopt::Long::Configure(qw(bundling noignore_case));
die unless GetOptions(
	"i|ignore-unknown+"	=> \$Opt_ignore_unknown,
	"s|sctp-only"		=> \$Opt_SCTP_only,
	"d|diameter-only"	=> \$Opt_DIAMETER_only,
	"S|print-skeleton"	=> sub
	{
		print <<'EOT';
#!/bin/sh

export tapcap_primary_local="";
export tapcap_secondary_local="";
export tapcap_primary_remote="";
export tapcap_secondary_remote="";

exec tapcap.pl "$@";
EOT
		chmod(0755, *STDOUT);
		exit;
	});

# Read %Machines from the %ENV:ironment.
while ((my ($key, $val) = each(%ENV)))
{
	next if $key !~ s/^tapcap_//;
	$Machines{$val} = $key;
}

# Work
if (!@ARGV)
{	# Process standard input.
	process(*STDIN);
} else
{	# Process files specified on the command line.
	foreach (@ARGV)
	{
		local *FH;

		# Filter *.cap files through tshark, read others as is.
		if (/\.p?cap\d*$/i)
		{
			open(FH, '-|', qw(tshark -n -r), $_, @tshark);
			process(*FH);
		} elsif (open(FH, '<', $_))
		{
			process(*FH);
		} else
		{
			die "$_: $!";
		}
		close(FH);
	}
}

# End of tapcap.pl
