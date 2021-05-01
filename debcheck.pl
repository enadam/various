#!/usr/bin/perl -w
#
# debcheck.pl -- verify the integrity of a manually downloaded .deb
#
# When you download a .deb file manually, dpkg -i doesn't verify its
# integrity, so you won't notice if it was tampered with on the mirror
# or during transmission.  This program looks up the .deb's expected
# hash in apt's database and verifies the file's actual hash against it.
# It also verifies that the package is what it claims to be by comparing
# the package name and version extracted from the file name with the
# corresponding fields in the package's control file.  On the first sight
# of a problem the program exits with non-zero status.
#
# Usage: debcheck.pl <.deb> ...

use strict;
use Dpkg::Control;

$\ = "\n";

# Process each package on the command line.
for my $fname (@ARGV)
{
	my ($pkg, $ver);
	my ($deb, $ref);
	my ($expected, $actual);

	# Extract the package's name and version from the file name.
	($pkg, $ver) = split(/_/, $fname);
	if (defined $ver)
	{	# apt-get download may encode ':' in the version.
		$ver =~ s/^(\d+)%3a/$1:/;
	} else
	{
		warn "Could not parse $fname";
	}

	# Extract the same information from the package's control file.
	open(DEB, "-|", ("dpkg", "-f", $fname, "Package", "Version"))
		or exit 1;

	# $deb->parse() dies if it can't parse the input.
	$deb = Dpkg::Control->new();
	$deb->parse(\*DEB);
	close(DEB) or exit 1;

	# Verify if we can that the package is what it claims to be.
	defined $$deb{'Package'}
		or die "$fname: control file has no Package field";
	defined $$deb{'Version'}
		or die "$fname: control file has no Version field";
	if (defined $ver) {
		$$deb{'Package'} eq $pkg
			or die "$fname: Package is not $pkg";
		$$deb{'Version'} eq $ver
			or die "$fname: Version is not $ver";
	}

	# Look up the package at the appropriate version in apt's database.
	open(APT, "-|", ("apt-cache", "show",
			"$$deb{'Package'}=$$deb{'Version'}"))
		or exit 1;

	$ref = Dpkg::Control->new();
	$ref->parse(\*APT);
	close(APT) or exit 1;

	# See which hash is available in $ref.
	if (defined ($expected = $$ref{'SHA256'}))
	{
		$actual = "sha256sum";
	} elsif (defined ($expected = $$ref{'MD5sum'}))
	{
		$actual = "md5sum";
	} else
	{
		die "$fname: $$deb{'Package'} at version $$deb{'Version'}"
			. "doesn't have either an SHA256 or an MD5sum";
	}

	# Compute $fname's $actual hash.
	$actual = qx($actual $fname);
	exit 1 if $?;
	chomp($actual);
	$actual =~ s/ .*$//;

	# Do they match?
	$actual eq $expected
		or die "$fname: hash mismatch";
	print "$fname: OK";
}

# End of debcheck.pl
