#!/usr/bin/perl -w
#
# lsmp3v2.pl -- display ID3v2 tags of MP3 files using id3v2(1)
#
# This program is very similar to lsmp3 in functionality and usage.
# The differences are that it displays v2 tags and it's less stable.
# The invocation and output are the same, though.

# Modules
use strict;
use IPC::Open2;

# Private functions
# Return the lines of $_[0], removing any possible line terminators.
sub read_file
{
	my $fname = shift;
	my @lines;
	local *FH;

	open(FH, '<', $fname)
		or die "$fname: $!";
	while (<FH>)
	{
		s/(?:\r\n|\n)$//;
		push(@lines, $_);
	}
	close(FH);

	return @lines;
}

# Print album/song info.
sub print_info
{
	my ($albuminfo, $author, $album, $title, $track, $genre, $year) = @_;

	if (!defined $author)
	{
		return;
	} elsif ($albuminfo)
	{
		$track =~ s/^([0-9])$/ $1/;
		print "$track. $author: $album ($year, $genre)"
	} else
	{
		$track =~ s/^([0-9])$/ $1/;
		print "$track. $title";
	}
}

# Main starts here.
my $albuminfo;
my ($author, $album, $title, $track, $genre, $year);

# Parse the command line.
if (@ARGV && $ARGV[0] eq "-a")
{
	$albuminfo = 1;
	shift;
}

# @ARGV <- MP3 files.
if (!@ARGV)
{
	if (-f "playlist")
	{
		@ARGV = read_file("playlist");
	} elsif (<*.m3u>)
	{
		@ARGV = read_file(<*.m3u>);
	} else
	{
		@ARGV = (<*.mp3>, <*.MP3>, <*.Mp3>);
	}
}

@ARGV or exit 0;
$\ = "\n";

# Run id3v2 through all MP3:s and read its output line by line.
defined open2(\*FH, undef, 'id3v2', '-R', @ARGV)
	or die "id3v2: $!";
while (!eof(FH))
{
	$_ = <FH>;
	chomp;

	if (/^Filename: /)
	{	# Start of description of a new file, flush the info
		# we've gathered.
		print_info($albuminfo, $author, $album,
			$title, $track, $genre, $year);
		$author = $album = $title = $track = $genre = $year = "";
	} elsif (/: No ID3v2 tag$/)
	{	# Likewise.
		print_info($albuminfo, $author, $album,
			$title, $track, $genre, $year);
		print $_;
		$author = $album = $title = $track = $genre = $year = undef;
	} elsif (s/^TPE1: //)
	{
		$author = $_;
	} elsif (s/^TALB: //)
	{
		$album = $_;
	} elsif (s/^TIT2: //)
	{
		$title = $_;
	} elsif (s/^TRCK: //)
	{
		$track = $_;
	} elsif (s/^TCON: //)
	{
		$genre = $_;
	} elsif (s/^TYER: //)
	{
		$year = $_;
	}
}

# Flush the info about the last file, then we're done.
print_info($albuminfo, $author, $album, $title, $track, $genre, $year);
exit(0);

# End of lsmp3v2.pl
