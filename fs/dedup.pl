#!/usr/bin/perl -w
#
# dedup.pl -- find files with the same content and hardlink them
#
# This program scans the specified directories and detects files with
# identical content, and tries to make them a hardlink instead, freeing
# up file system space.  It works by calculating MD5 hashes of files
# and checking to see whether it has seen the same hash before.
#
# Important features are:
# -- handling MD5 collisions: it does a byte-by-byte comparison before
#    deeming two files identical
# -- handling already linked files: if there are eg. three and four
#    already hardlinked files, the former three files will be linked
#    to the latter four
# -- handling files across different file systems: it does not attempt
#    to link files residing on different file systems together
# -- taking file system operations carefully: it tries hard not to mess up
#    the file system in error situations
# -- able to handle arbitrarily large amount of files (as long as memory
#    allows)
# -- doing as few file system operations as possible (since they are a
#    possible source of failure)
#
# Synopsis:
#   dedup.pl [<options>...] [<files-and-directories>...]
#
# Options:
#   -v				Tell which file is being processed.
#   -n				Don't modify the file system, just print
#				what would have been done.
#   -i {<extension>|<regexp>}	Include these files in the set of files
#				considered for deduplication, and ignore
#				any others.  The argument is taken as an
#				<extension> if it entirely consist of
#				alphanumeric characters.
#   -x {<extension>|<regexp>}	Do not scan matching files.  First the -i
#				list is matched, then the -x list.
#
# By default, if no <files-and-directories> are given the current directory
# is scanned.

# Modules
use strict;
use IPC::Open2;
use File::Find;
use Getopt::Long;
use POSIX qw(ENOENT EEXIST EISDIR);

# Private variables
# $Opt_dry_run conforms to the -n command line option.
# $Erred decides whether we'll exit with an error code (had an error) or not.
my ($Opt_dry_run, $Erred);

# Private functions
# Make hardlinks of $src to each of @$dst in a safe manner.
# If any of @$dst exists, it will be overwritten.  Sets $Erred
# if there was an error.
sub lnfiles
{
	my ($src, $dst) = @_;

DEST:	foreach (@$dst)
	{
		my $tmp;

		# Print what we're about to do.
		print "ln -f '$src' '$_';";
		next if $Opt_dry_run;

		# First move $_ (the current destination) away to a temporary
		# file, so if linking of $src fails, we can restore things.
		# Try until we find an unique file name.
		for (;;)
		{
			# Create a temporary file name by appending a random
			# value to the destination file name.
			$tmp = "$_." . int(rand(1_000_000));
			if (rename($_, $tmp))
			{	# Success, we can proceed with linking.
				last;
			} elsif ($! == ENOENT)
			{	# $_ doesn't even exist, that's also okay.
				undef $tmp;
				last;
			} elsif ($! != EEXIST && $! != EISDIR)
			{	# Renaming failed for some reason other than
				# we happened to choose an existing $tmp name.
				# That's not ok.
				print "rename($_, $tmp): $!";
				$Erred = 1;
				next DEST;
			}
		}

		# Try linking.
		if (!link($src, $_))
		{
			print STDERR "$0: ln($src, $_): $!";
			$Erred = 1;

			# Try to restore $_ if necessary.
			!defined $tmp || rename($tmp, $_)
				or print "$0: rename($tmp, $_): $!";
		} elsif (defined $tmp && !unlink($tmp))
		{	# Linking succeeded, but cleaning up $tmp failed.
			print STDERR "$0: $tmp: $!";
			$Erred = 1;
		}
	}
}

# Returns whether $str matches any of the rest of the arguments,
# which are supposed to be regular expressions.
sub matched
{
	my $str = shift;

	$str =~ $_ and return 1
		foreach @{$_[0]};

	return 0;
}

# Print usage then exit.
sub usage
{
	my $iserr = shift;
	my $fh = $iserr ? *STDERR : *STDOUT;

	print $fh "usage: $0 [-vn] [-i <include>] [-x <exclude>] ",
		"[<files-or-directories>...]\n";
	exit($iserr || 0);
}

# Main starts here
# %fnames is a mapping between file names and inode keys (device number
# + inode number).  These keys are supposed to identify any file on the
# system globally uniquely.
#
# @fnames simply enumerates the files to operate on.
#
# %inodes is a mapping between inode keys and file names having the same
# content.  Its value is a list of list, each inner list consisting of
# the file names with the same inode number.  So, for example a value of
# [ [ qw(alpha beta gamma) ], [ qw(one two) ] ] means all of the file have
# the same content, and alpha, beta and gamma are links (have the same inode
# number, just like one and two, but the two set of files have different
# inode numbers.  If these files reside on the same file system, they should
# be linked together.
#
# %sizes is a mapping between file sizes and file names with that size.
#
# %cksums is a mapping between MD5 hashes and inode keys.  Its value is
# a list of inode keys, each of them from a different device.  It's used
# to store "master" inode keys: files with a given MD5 hash will be compared
# with the master inode (retrieved from %inodes).
#
# The program has four phases: first the files to be consdered are collected.
# Then we eliminate files with unique sizes (thus which cannot be an exact
# copy of other files).  Following that, the files which could be linked
# are figured out.  Finally, the appropriate files are linked together in
# the most efficient manner.
my (%fnames, @fnames, %inodes, %sizes, %cksums);
my ($opt_verbose, @opt_include, @opt_exclude);

# Parse the command line.
Getopt::Long::Configure(qw(gnu_compat permute bundling noignore_case));
usage(1) unless GetOptions(
	'h|help'	=> sub { usage() },
	'v|verbose'	=> \$opt_verbose,
	'n|dry-run'	=> \$Opt_dry_run,
	'i|include=s'	=> sub
       	{	# Push compiled regexps into @opt_include and @opt_exclude.
		# If the argument is entirely alphanumeric, take it as a
		# file name extension.
		push(@opt_include, $_[1] =~ /^[[:alnum:]]*$/
			? qr/\Q.$_[1]\E$/ : qr/$_[1]/);
	},
	'x|exclude=s'	=> sub
       	{	# Likewise.
		push(@opt_exclude, $_[1] =~ /^[[:alnum:]]*$/
			? qr/\Q.$_[1]\E$/ : qr/$_[1]/);
	});

# Phase I.
# Collect the file names we'll operate on, and initialize %fnames and %inodes.
# Scan directories recursively.  If no @ARGS are given, consider the current
# directory.
find({
	# Don't allow File::find() to change directories to save system calls
	# and to prevent situations when we couldn't go back to back to the
	# current directory, and File::find() can do its job excellently
	# without it anyway.
	no_chdir => 1,
	wanted => sub
	{
		my ($dev, $ino, $size, $key);

		# File name already seen, probably specified twice on the
		# command line.
		return if exists $fnames{$_};

		# Match $basename against patterns in @opt_include and
		# @opt_exclude, and skip it if it doesn't satisfy the
		# requirements.
		if (@opt_include || @opt_exclude)
		{
			my $basename;

			$basename = $_;
			$basename =~ s!^/+$!/!
				or $basename =~ s!^.*/+!!;
			return if @opt_include && !matched($basename,
				\@opt_include);
			return if @opt_exclude &&  matched($basename,
				\@opt_exclude);
		}

		# Skip everything other than readable regular files.
		($dev, $ino, undef, undef, undef, undef, undef, $size) = stat;
		return if ! -f _ || -l _ || ! -r _;

		# The $key is the unique identifier of an inode.
		$key = "${dev}_${ino}";
		if (!exists $inodes{$key})
		{	# We haven't met this inode yet.
			$inodes{$key} = [ [ $_ ] ];
			push(@{$sizes{$size}}, $_);
			$fnames{$_} = $key
		} else
		{	# $_ is a hardlink of an already seen file.
			push(@{$inodes{$key}->[0]}, $_);
		}
	},
}, @ARGV ? @ARGV : '.');

# Phase II.
# Eliminate %fnames with unique %sizes, then clear the hash.
while (my (undef, $fnames) = each(%sizes))
{
	delete $inodes{delete $fnames{$$fnames[0]}}
		if @$fnames == 1;
}
undef %sizes;

# Process @fnames in alphabetic rather than some totally alphabetic orders,
# as files in the same directory are more likely placed together.  If we
# didn't gather any @files, we've got nothing to do.
@fnames = sort(keys(%fnames));
exit if !@fnames;

# Phase III.
# Get the MD5 hash of @files and see which files can be linked together.
# Communicate with md5sum though xargs for maximum performance (ie. we
# invoke md5sum as few times as possible).
$| = 1;
defined open2(\*MD5SUM, \*XARGS, 'xargs', '-0', '--', 'md5sum', '-b')
	or die;
for (;;)
{
	my ($fname, $cksum, $key, $dev);
	my ($keys, $key2, $fnames, $fnames2);

	# Pump @fnames to XARGS until we can read results from MD5SUM.
	while (@fnames)
	{
		my $rfs;

		print XARGS shift(@fnames), "\0";
		if (!@fnames)
		{	# Out of @fnames, send EOF to XARGS.
			close(XARGS);
			last;
		}

		# Anything to read from MD5SUM?
		$rfs = '';
		vec($rfs, fileno(MD5SUM), 1) = 1;
		last if select($rfs, undef, undef, 0);
	}

	# Get the $fname and $cksum from the line read from MD5SUM.
	defined ($_ = readline(MD5SUM))
		or last;
	/^([0-9a-f]{32}) \*(.*)$/ or die;
	($fname, $cksum) = ($2, $1);
	print $fname, "\n" if $opt_verbose;
	defined ($key = delete $fnames{$fname})
		or die;

	# $dev:ice number will be used to recognize files with the same
	# content but unfortunately residing on different file systems.
	# In this case we can't link them together.
	($dev) = $key =~ /^(\d+)_/;
	defined $dev or die;

	if (!defined ($keys = $cksums{$cksum}))
	{	# We haven't encountered this hash yet.
		$cksums{$cksum} = [ $key ];
		next;
	}

	# Have we seen any files with the same $cksum from the same device?
	foreach (grep(/^${dev}_/, @$keys))
	{
		die if defined $key2;
		$key2 = $_;
		die if $key eq $key2
	}

	if (!defined $key2)
	{	# We haven't encountered any files with this $cksum
		# on this $dev:ice.
		push(@$keys, $key);
		next;
	}

	# Get the list of $fnames which can potentially be linked together
	# because of the same $cksum and $dev:ice.
	defined ($fnames = $inodes{$key})
			&& @$fnames
			&& @{$$fnames[0]}
		or die;
	defined ($fnames2 = $inodes{$key2})
			&& @$fnames2
			&& @{$$fnames2[0]}
		or die;
	if (system("cmp", $$fnames[0][0], $$fnames2[0][0]) != 0)
	{	# Despite the same $cksum, the files are different.
		print "MD5 collision between $$fnames[0][0] ",
			"and $$fnames2[0][0]\n";
	} else
	{	# @$fnames and @$fnames2 can be linked together.
		push(@$fnames2, @$fnames);
		delete $inodes{$key};
	}
}
close(MD5SUM);

# We should have consumed all of %fnames.
die if %fnames;

# We don't need %cksums any more.
undef %cksums;

# Phase IV.
# Do the actual linking.
$\ = "\n";
foreach my $inode (values(%inodes))
{
	my ($which, $max);

	# All of @$inodes can be linked together, but it does matter
	# which file we'll link to another.  Ie. if there are three
	# files in one cluster (already linked together) and four in
	# the other, we can save syscalls by the former to the latter,
	# simply because there're less files to link.  Find out $which
	# is the largest cluster (the most files within).
	foreach (@$inode)
	{
		if (!defined $which || $max < @$_)
		{
			$which = $_;
			$max = @$_;
		}
	}

	# Now we really know what to link where.  Link all files except
	# those in @$which with $$which[0] (which is the same as $$which[1],
	# etc).
	$_ == $which
		or lnfiles($$which[0], $_)
		foreach @$inode;
}

# Done
exit defined $Erred;

# End of dedup.pl
