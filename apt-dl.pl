#!/usr/bin/perl -w
#
# apt-dl.pl -- download and optionally extract Debian packages
#
# Synopsis:
#   apt-dl.pl	[-u | -X | {-x <what-to-extract>}...]
#		[-d <output-directory>] <package>...
#
# apt-dl.pl downloads .deb files from Debian repositories and optionally
# extract (perhaps part of) their file system contents.
#
# Options:
#   -u		Just print the URL of the <package>
#   -d <dir>	Download/extract into <dir>, creating it if necessary.
#   -X		Extract the whole file system.
#   -x <path>	Extract just <path>:s from the file system.
#		Can be specified multiple times.
#
# apt-dl.pl can handle file, copy, http, https and ftp repositories
# (plus any other supported by wget(1)).
#

# Modules
use strict;
use File::Copy;
use POSIX qw(EEXIST);

# Constants
use constant AR_MAGIC	=> "!<arch>\n";
use constant AR_HEADER	=> 16 + 12 + 6 + 6 + 8 + 10 + 2;

# Program code
sub usage
{
	print STDERR "usage: $0 "
		. "[-u | -X | {-x <what-to-extract>}...] "
		. "[-d <output-directory>] <package>...\n";
	exit(1);
}

# Checks whether $input is the beginning of an ar(1) archive.
sub ar_check
{
	my $input = shift;
	my ($data, $read);

	# Read the ar(1) magic.
	defined ($read = read($input, $data, length(AR_MAGIC)))
			&& $read > 0
		or return 0;
	$data eq AR_MAGIC
		or die "file is not in ar(1) format";

	return 1;
}

# Find $member in $input (which is supposed to be in ar(1) format)
# and print it to $output.
sub ar
{
	my ($input, $output, $member) = @_;
	my ($data, $size, $read);

	# File header is assumed to have been consumed by ar_check().

	# For each member ...
	for (;;)
	{
		# Read the member header.
		defined ($read = read($input, $data, AR_HEADER))
			or die "read: $!";
		$read > 0
			or die "$member not found in archive";
		$read == AR_HEADER
			or die "file is not in ar(1) format";

		# Parse the header.
		$data =~ /^(.{16})	# member name
			    .{12}	# date
			    .{6}.{6}	# UID and GID
			    .{8}	# mode
			   (.{10})	# size
			   (.{2})	# magic
			   $/sx
			or die "file is not in ar(1) format";
		$3 eq "`\n"
			or die "file is not in ar(1) format";

		# Is it $member?
		$size = 0+$2;
		last if $1 =~ /^\Q$member\E\s*$/;

		# No, skip to the next member.
		# File data is 2-bytes aligned.
		$size++ if $size % 2;
		for (; $size > 0; $size -= $read)
		{
			defined ($read = read($input, $data,
					$size > 4096 ? 4096 : $size))
				or die "read: $!";
			$read > 0
				or die "unexpected end of file";
		}
	}

	# Read $member and print it to $output.
	local $\ = "";
	for (; $size > 0; $size -= $read)
	{
		defined ($read = read($input, $data,
				$size > 4096 ? 4096 : $size))
			or die "read: $!";
		$read > 0
			or die "unexpected end of file";
		print $output $data;
	}

	return 1;
}

# Main starts here
my ($opt_url_only, $opt_extract, @opt_extract_paths, $opt_outdir);
my (@repos, $package, $version, $path);

# Parse the command line.
while (@ARGV)
{
	if ($ARGV[0] eq '-u')
	{
		shift;
		$opt_url_only = 1;
	} elsif ($ARGV[0] eq '-X')
	{
		shift;
		$opt_extract = 1;
	} elsif ($ARGV[0] eq '-x')
	{
		shift;
		@ARGV or usage();
		$_ = shift;
		unless (m{^\./})
		{
			my $prefix = m{^/} ? "." : "./";
			$_ = $prefix . $_;
		}
		push(@opt_extract_paths, $_);
		$opt_extract = 1;
	} elsif ($ARGV[0] eq '-d')
	{
		shift;
		@ARGV or usage();
		$opt_outdir = shift;
	} elsif ($ARGV[0] =~ /^-/)
	{
		usage();
	} else
	{
		last;
	}
}

# Nothing to do if no packages are specified.
if (!@ARGV)
{
	warn "No packages specified";
	exit 0;
}

defined $opt_outdir
	or $opt_outdir = '.';

# Discover possible repositories to download from.
for my $sources_list ("/etc/apt/sources.list", </etc/apt/sources.list.d/*.list>)
{
	-f $sources_list or next;
	open(FH, '<', $sources_list)
		or die "$sources_list: $!";
	while (<FH>)
	{
		my $uri;

		/^\s*deb\s+(\S+)\s+([\w\/]+)(?:\s+[\w\-]+)*\s*$/ or next;
		$uri = $1;

		if ($2 =~ m!/!)
		{
			my $path = $2;

			$uri =~ s!/+$!!;
			$path =~ s!^/+~!!;
			$path =~ s!/+$!!;
			$uri = "$uri/$path";
		}

		push(@repos, $uri);
	}
	close(FH);
}

@repos or die "couldn't identify any repository";

# Create and chdir to $opt_outdir.
if (!$opt_url_only && $opt_outdir ne ".")
{
	mkdir($opt_outdir)
		or $! == EEXIST
		or die "$opt_outdir: $!";
	chdir($opt_outdir)
		or die "$opt_outdir: $!";
}

# Download and extract each package.  Acquire the concrete filename from
# apt-cache(1).
$"= " ";
$\ = "\n";
open(APT_CACHE, '-|', qw(apt-cache show --no-all-versions), @ARGV)
	or die "apt-cache: $!";
PACKAGE: while (<APT_CACHE>)
{
	if (s/^Package:\s*//)
	{	# Save the package name under processing.
		chomp;
		$package = $_;
		next;
	} elsif (s/^Version:\s*//)
	{	# Find out $package's location in the repository.
		chomp;
		$version = $_;
		next;
	} elsif (s/^Filename:\s*//)
	{	# Find out $package's location in the repository.
		chomp;
		$path = $_;
		next;
	} elsif ($_ eq "\n")
	{	# End of Package: record.  We can start working now.
	} else
	{
		next;
	}

	# Action
	REPO: foreach (@repos)
	{
		my $repo = $_;

		if ($opt_url_only)
		{
			if ($repo =~ s!^(?:file|copy)://!!)
			{
				$path = "$repo/$path";
				-f $path or next REPO;
				print $path;
			} else
			{
				print "$repo/$path";
			}
		} elsif (!$opt_extract)
		{
			if ($repo =~ s!^(?:file|copy)://!!)
			{	# Just copy it if it's on localhost.
				$path = "$repo/$path";
				-f $path or next REPO;
				copy($path, ".")
					or die "$path: $!";
			} else
			{
				my $url = "$repo/$path";
				system("wget", $url);
				if ($?)
				{	# wget exits with 8 if it cannot find
					# the file to be downloaded.
					$? >>= 8;
					$? == 8 ? next REPO : exit $?;
				}
			}
		} elsif ($repo =~ s!^(?:file|copy)://!!)
		{
			$path = "$repo/$path";
			-f $path or next;
			system("ar p \"\Q$path\E\" | tar xz");
			exit $? >> 8 if $?;
		} else
		{
			my $url = "$repo/$path";

			# We have to extract data.tar.gz from the downloaded
			# .deb manually, because ar(1) doesn't work with stdin.

			# First verify that we have the correct $repo in order
			# to avoid unnecessary errors from tar and gzip.
			open(WGET, '-|', qw(wget -q -O -), $url)
				or die "wget: $!";
			if (!ar_check(*WGET))
			{
				close(WGET);
				$? >>= 8;
				$? == 8 ? next REPO : exit $?;
			}

			# Now we can be sure about the $package's location.
			print STDERR defined $version
				? "Getting $package ($version) from $repo..."
				: "Getting $package from $repo...";
			open(TAR, '|-', qw(tar xz), @opt_extract_paths)
				or die "tar: $!";
			ar(*WGET, *TAR, 'data.tar.gz');
			close(WGET) or $! ? die "wget: $!" : exit $? >> 8;
			close(TAR)  or $! ? die "tar: $!"  : exit $? >> 8;
		}

		# Remove $package from the list of packages to process.
		@ARGV = grep($_ ne $package, @ARGV);
		$package = $version = $path = undef;
		next PACKAGE;
	}

	die  "Couldn't locate $package";
}

# Done
close(APT_CACHE);
if (@ARGV)
{
	print STDERR "Couldn't find @ARGV.";
	exit 1;
}

exit 0;

# End of apt-dl.pl
