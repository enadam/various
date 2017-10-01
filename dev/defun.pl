#!/usr/bin/perl -w
#
# defun.pl -- shorten full-blown method declarations in a log file
#
# gcc's __PRETTY_FUNCTION__ gives a very detailed function signature,
# which (while being precise) makes it harder to read log files.
# This program detects such function signatures and replaces them
# with a simple method name.
#
# The other function of this program is to make it easier to extract
# log records from monitor outputs.
#
# Usage:
#   defun.pl [-kUl] [-x <string>]... [<logile>]...
#
# Options:
#   -k		keep the class names of methods
#   -l		prepend "ipdu_0_dia_logs.log" to the list of files
#		to be filtered, and pipe output to $PAGER
#   -U		do not sort and remove duplicate log records
#   -x <string>	omit records containing <string>, eg. MMT_HEARTBEAT
#

# Modules
use strict;

# Constants
my $DATE_TIME = qr/\d{4}-\d{2}-\d{2} \s+\d{2}:\d{2}:\d{2}\.\d+/x;

# Private variables
my ($Opt_keepclass, $Opt_sort_uniq, @Opt_exclude);

# Program code
# sort -u @$records.
sub sort_uniq_print
{
	my $records = shift;
	my $date_time;
	my ($prev_header, $prev_reduced_header, $prev_log, $ndupes);

	return if !@$records;

	# Sort the @$records by date and time.
	$date_time = qr/\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+/;
	@$records = sort({
		my ($lhs) = $$a[0] =~ /($date_time)/o;
		my ($rhs) = $$b[0] =~ /($date_time)/o;
		return $lhs cmp $rhs;
	} grep(defined, @$records)); 

	# ($prev_header, $prev_log) <- first record
	# $prev_reduced_header <- $prev_header without the sequence number.
	# We'll reduce @$records by comparing the subsequent reduced headers.
	($prev_header, $prev_log) = @{shift(@$records)};
	$prev_reduced_header = $prev_header;
	$prev_reduced_header =~ s/^\s*\d+\s+//;
	$ndupes = 1;

	# Reduce and print @$records.
	foreach (@$records)
	{
		my ($header, $reduced_header, $log);

		($header, $log) = @$_;
		$reduced_header = $header;
		$reduced_header =~ s/^\s*\d+\s+//;

		if ($prev_reduced_header ne $reduced_header
			|| $prev_log ne $log)
		{	# This record differs from the previous one.
			print $ndupes > 1
		       		? "$prev_header ($ndupes times)"
				: $prev_header, $prev_log;
			($prev_header, $prev_reduced_header, $prev_log)
				= ($header, $reduced_header, $log);
			$ndupes = 1;
		} else
		{	# This record is the same as the previous one.
			$ndupes++;
		}
	}

	# Flush $prev_*.
	print $ndupes > 1
		? "$prev_header ($ndupes times)"
		: $prev_header, $prev_log;
}

# Take a string like "file.ext:123: <signature> <message>" and reduce
# <signature> to the minimum.
sub decppnoise
{
	my $log = shift;
	my ($file_line, $class, $fun, $msg);

	# Parse file name and line number.
	$log =~ s/^(.*?:\d+)\s*//
		or die "`$log'";
	$file_line = $1;

	if ($log =~ s/^://)
	{
		# Parse class name and method.
		$log =~ s/^.*?(?:(\w+)(?:<.+?>)?::)*(~?\w+)\(/(/
			or die "`$log'";
		($class, $fun) = ($1, $2);
		$log =~ s/( \( (?: (?>[^()]+) | (?1) )* \) )//x
			or die "`$log'";

		# Remove trailing "const" and template arguments.
		$log =~ s/^(?:\s*const\b)?(?:\s*\[with .*?\])?//;
	}

	$msg = $log;
	$msg =~ s/^\s+//;

	$log  = "$file_line: ";
	if (defined $fun)
	{
		$log .= "${class}::"
			if $Opt_keepclass && defined $class;
		$log .= "$fun(): $msg";
	} else
	{
		$log .= $msg;
	}

	return $log;
}

# Return a [ $header, $log ].  C++ method signatures are removed from
# $header if it is written by LBManager or LBSDiaCore.  $log is indented
# so that it lines up with $header.
sub mkrecord
{
	my ($header, $indent) = (shift, shift);
	my $log;

	$log = join(' ', @_);
	index($log, $_) >= 0 and return undef
		foreach @Opt_exclude;

	if ($header =~ /\s+(?:LBManager|LBSDIACore)(?:\.\w+)?:\s+PID:\d+$/
		|| $header =~ /^\s*\d+\s+PID:\d+\b/)
	{	# Perform method-signature reduction.
		$log = decppnoise($log);
	} else
	{
		$log =~ s/^\s+//;
	}

	return [ $header, (' ' x $indent) . $log];
}

# Main starts here
my (@records, $header, $indent, @loglines);
my $opt_usual;

# Parse the command line.
$Opt_sort_uniq = 1;
while (@ARGV)
{
	if ($ARGV[0] eq '-k')
	{
		$Opt_keepclass = 1;
		shift;
	} elsif ($ARGV[0] eq '-l')
	{
		$opt_usual = 1;
		shift;
	} elsif ($ARGV[0] eq '-U')
	{
		$Opt_sort_uniq = 0;
		shift;
	} elsif ($ARGV[0] eq '-x')
	{
		shift;
		@ARGV or die "required parameter missing";
		push(@Opt_exclude, shift);
	} else
	{
		last;
	}
}

if ($opt_usual)
{
	unshift(@ARGV, 'ipdu_0_dia_logs.log');
	open(STDOUT, '|-', $ENV{PAGER})
		or die;
}

# Run, collect @records.
# We're inside a log print if and only if $header is not undef.
# Then we collect everything in @loglines between two $header:s,
# and finally add a new log record when we see the next $header,
# or if we reach the end of the file.
for (;;)
{
	$_ = <>;
	if (defined $_ && s/^((\s*\d+\s+)PID:\d+\s+\w+\s+$DATE_TIME)\s+//)
	{	# 0 PID:6329 DBG 2013-05-07 09:56:32.352013 main.cpp:...
		# Module test log.  Make it a two-line record.
		chop;
		push(@records, mkrecord($1, length($2), $_));
		#print $1, "\n", ' ' x length($2), decppnoise($_);
	} elsif (!defined $_ || /^\d+-\$/ || $_ eq "COMMAND EXECUTED\r\n")
	{	# End of log printout (EOF or command prompt).
		# Flush the pending log record, then go $outside.
		push(@records, mkrecord($header, $indent, @loglines))
			if defined $header;
		last if !defined $_;
		$header = undef;
		@loglines = ();
	} elsif (s/^((\s*\d+
		 \s+)\w-\d{3}
		 \s+$DATE_TIME
		 \s+[\w.-]+:
		 (?:\s+PID:\d+)?)//ox)
  	{ # "  262 E-000  2013-04-19 07:18:12.63  LBSDIACore: PID:1784"
		my ($next_header, $next_indent, $next_log);

		# Parse a new header.
		($next_header, $next_indent) = ($1, length($2));
		s/^\s+//;
		s/\s+$//;
		$next_log = $_;

		# Flush current log record.
		push(@records, mkrecord($header, $indent, @loglines))
			if defined $header;

		# Start a new log record.
		($header, $indent) = ($next_header, $next_indent);
		@loglines = $next_log ne "" ? ($next_log) : ();
	} elsif (defined $header)
	{	# Collect everything between $header:s in @loglines.
		s/^\s+//;
		s/\s+$//;
		push(@loglines, $_);
	}
}

# Done, print @records.
$, = $\ = "\n";
if ($Opt_sort_uniq)
{
	sort_uniq_print(\@records);
} else
{
	print @$_ foreach grep(defined, @records);
}

close(STDOUT);

# End of defun.pl
