#!/usr/bin/perl -w
#
# gimme.pl -- tiny HTTP server for sharing personal stuff
#
# Just start it up and let friends browse and download your public goodies.
# The exported directories will be browseable and downloadable as tarballs.
#
# Synopsis:
# ./gimme.pl [-C] [<address> | <dir>]
# ./gimme.pl [-C]  <address>   <dir>
#
# <dir> designates the wwwroot of the service.  Clients will be able to get
# everything there, but nothing outside.  You can hide stuff from the index
# by placing them in a dotfile or dotdir.  They will remain browseable and
# downloadable with a direct link, though.  You can put a .motd file in the
# wwwroot to display it in all directory listings.
#
# <address> is the interface address to listen on (localhost by default,
# use 'all' to listen on all interfaces), and <dir> is the root of the
# directory to be served (the current working directory by default).
# If started as root gimme tries to listen on port 80 (otherwise on
# port 8080) and then it drops all root privileges.
#
# If the -C flag is present gimme chroot()s to <dir>.  This will disable
# directory downloading unless you make tar available in the chroot.
# Generally speaking, the chrooted mode may be quite fragile because
# it is hard to predict what kind of modules will LWP and the others
# require in run-time.  We're doing our best, though.
#
# gimme re-executes iteself when it receives a SIGHUP, pleasing its
# developer and the uptime whores.
#
# gimme.pl was written with public (but not wild) exposure in mind,
# so it is thought to be relatively secure in the sense that it won't
# give away stuff you didn't intend to share.  However, it can be DoS:ed
# and generally, is not efficient with large files.
#

# Program code
# Encapsulates a Unix path and tries to provide a sensible way to access
# and change the individual components for various purposes, and to
# stringify the path regardless its absoluteness.
package Path; # <<<
use strict;
use overload '""' => \&as_string;

# Internal representaion: we are an array which stores the path elements.
# The first element indicates whether we're absolute ('/') or relative.
# The rest of the elements shouldn't be '.' and should not contain '/'s
# and should not be empty strings.  Otherwise the user is free to change
# the contents anytime by any means.
sub be
{
	my ($class, $is_absolute) = (shift, shift);
	return bless([$is_absolute ? '/' : '.', @_], ref $class || $class);
}

sub new
{
	my ($class, $path) = @_;
	my ($is_absolute, @path);

	if (defined $path && $path ne '')
	{
		$is_absolute = $path =~ s{^/+}{};
		@path = grep($_ ne '.', split(qr{/+}, $path));
	}

	return ref $class && !$is_absolute
		? $class->be($class->is_absolute(), $class->dirs(), @path)
		: $class->be($is_absolute,                          @path);
}

sub dirs
{
	my $self = shift;
	return @$self[1..$#$self];
}

sub is_absolute
{
	shift->[0] eq '/'
}

sub as_absolute
{
	my $self = shift;
	$self->be(1, $self->dirs())
}

sub as_relative
{
	my $self = shift;
	$self->be(0, $self->dirs())
}

sub as_string
{
	my $self = shift;

	if (@$self == 1)
	{
		return $$self[0];
	} elsif ($self->is_absolute())
	{
		return join('/', '', $self->dirs());
	} else
	{
		return join('/',     $self->dirs());
	}
}

sub add
{
	my $self = shift;
	push(@$self, @_);
	return $self;
}

sub pop
{
	my $self = shift;
	CORE::pop(@$self) if @$self > 1;
	return $self;
}

sub clear
{
	my $self = shift;
	$#$self = 0;
	return $self;
}

sub absolutely_clear
{
	my $self = shift;
	@$self = qw(/);
	return $self;
}
# End of module Path >>>

# Override Daemon::ClientConn to notify the client we'll close the connection
# after the response.
package GoodByeClient;

our @ISA = qw(HTTP::Daemon::ClientConn);
use strict;

sub send_basic_header
{
	my $self = shift;
	$self->SUPER::send_basic_header(@_);
	print $self "Connection: close\r\n";
}

package main;
use strict;
use Errno qw(ENOENT ENOTDIR EPERM EACCES);
use HTTP::Daemon;
use HTTP::Status;

# For chrooted mode.
use File::Basename;
use LWP::MediaTypes;

# Default file name for 'gimmegimme'.
my $GIMME = $0;
$GIMME =~ s!^.*/+!!;

# Default file name of the .tgz of the site.
my $SITE = `hostname`;
chomp($SITE);

# Don't tire these user agents with navigation bar or advertisement.
my $ROBOTS = qr/\b(?:wget|googlebot)\b/i;

# HTML generation <<<
sub escape
{
	my $str = shift;

	$str =~ s/&/&amp;/g;
	$str =~ s/</&lt;/g;
	$str =~ s/>/&gt;/g;

	return $str;
}

sub html	{ "<HTML>\n<HEAD><TITLE>" . escape(shift) . "</TITLE></HEAD>\n"
		. "<BODY>\n@_</BODY>\n</HTML>" }
sub table	{ "\t<TABLE border=\"0\">\n@_\t</TABLE>\n"	}
sub row		{ "\t\t<TR>\n@_\t\t</TR>\n"			}
sub cell	{ "\t\t\t<TD>$_[0]</TD>\n"			}
sub right	{ "\t\t\t<TD align=\"right\">$_[0]</TD>\n"	}
sub para	{ "\t<P>@_</P>\n"				}
sub color	{ sprintf('<FONT color="#%.2X%.2X%.2X">%s</FONT>', @_) }

sub mklink
{
	my ($a, $title, $href, $q) = @_;

	if (defined $href)
	{
		$href = URI->new($href);
		$href->query($q) if defined $q;
	} else
	{
		$href = URI->new($a);
	}

	$a = escape($a);
	if (defined $title)
	{
		$title = escape($title);
		return "<A href=\"$href\" title=\"$title\">$a</A>";
	} else
	{
		return "<A href=\"$href\">$a</A>";
	}
}
# HTML generation >>>

# Functionality <<<
sub read_chunk
{
	my $fh = shift;
	my $buf;

	return read($fh, $buf, 4096) ? $buf : undef;
}

sub check_path
{
	my ($client, $path) = @_;

	if (stat($path))
	{
		if (-f _)
		{
			return 1 if -r _;
		} elsif (-d _)
		{
			return 1 if -r _ && -x _;
		}
		$client->send_error(RC_FORBIDDEN)
	} elsif ($! == ENOENT || $! == ENOTDIR)
	{
		$client->send_error(RC_NOT_FOUND);
	} elsif ($! == EPERM || $! == EACCES)
	{
		$client->send_error(RC_FORBIDDEN);
	} else
	{
		$client->send_error(RC_INTERNAL_SERVER_ERROR);
	}
	return 0;
}

sub send_tar
{
	my ($client, $path) = @_;
	my $dir;

	# The last component is expected to be the suggested file name.
	$path->pop();
	check_path($client, $path)
		or return;
	if (! -d _)
	{
		$client->send_error(RC_NOT_FOUND);
		return;
	}

	# What to transform the initial '.' of the paths in the archive to.
	# This should be the directory name we made the archive of or $SITE.
	$dir = $path->dirs() ? $$path[-1] : $SITE;
	$dir =~ s/\\/\\\\/g;
	$dir =~ s/!/\\!/g;

	# Make tar(1) transform the path prefixes to $dir.
	open(TAR, '-|', qw(tar cz), '-C', $path,
		'--transform', "s!^\\.\$!$dir!;s!^\\./!$dir/!", '.')
		or return $client->send_error(RC_SERVICE_UNAVAILABLE);
	$client->send_response(HTTP::Response->new(
		RC_OK, 'Here you go',
		[ 'Content-Type' => 'application/x-tar' ],
		sub { read_chunk(*TAR) }));
	close(TAR);
}

sub get_index
{
	my $path = shift;
	my %index;
	local *INDEX;

	# Read the %index of file descriptions in this directory.
	if (open(INDEX, '<', $path))
	{
		while (<INDEX>)
		{
			my ($fname, $desc);

			chomp;
			($fname, $desc) = split(' ', $_, 2);
			$index{$fname} = $desc if defined $desc;
		}
		$index{'00INDEX'} = 'file descriptions'
			unless exists $index{'00INDEX'};
	}

	return \%index;
}

sub send_dir
{
	my ($client, $request, $path) = @_;
	my ($ua, $isrobi, $location);
	my ($motd, $navi, @upper, @lower);
	my (@list, $list, $ad);
	my ($index, $desc);

	# Is $client a robot?
	$isrobi = defined ($ua = $request->header('User-Agent'))
		&& $ua =~ $ROBOTS;

	# Upper navigational bar
	$location = Path->new('/');
	if (!$isrobi)
	{
		push(@upper, mklink($location, 'Site root'));
		push(@lower, mklink('Gimme!', 'Download site',
					"/$SITE.tar.gz", 'gimme'));

		my @dirs = $path->dirs();
		while (@dirs)
		{
			my $dir = shift(@dirs);
			$location->add($dir);
			push(@upper, mklink($dir, @dirs > 0
				? 'Go to upper directory'
				: 'This directory',
				$location));
			push(@lower, mklink('Gimme!', @dirs > 0
				? 'Download tree'
				: 'Download this directory',
				"$location/$dir.tar.gz", 'gimme'));
		}

		$navi = table(
			row(map({ cell($_) } @upper)),
			row(map({ cell($_) } @lower)));
	} else
	{	# $location <- all but the last element
		$location->add($_) foreach $path->dirs();
	}

	# Get $motd.
	if (open(MOTD, '<', '.motd'))
	{
		local $/ = "";
		$motd  = '';
		$motd .= para(escape(/(.*)/s)) foreach <MOTD>;
		close(MOTD);
	}

	# Get this directory's description from the parent's INDEX.
	if ($path->dirs())
	{
		$index = get_index($path->new()->pop()->add('00INDEX'));
		$desc = $$index{$$path[-1]};
		$desc = defined $desc ? para($desc) : '';
	} else
	{
		$desc = para("This is ${SITE}'s root directory.");
	}

	# Description of the stuff in this directory.
	$index = get_index($path->new('00INDEX'));

	# Creare the directory @list.
	while (<\Q$path\E/*>)
	{
		my (@row, $link, $full);

		$link = readlink($_);
		stat($_)
			or defined $link && lstat($_)
			or next;

		s!^.*/+!!;
		$full = $location->new($_);

		push(@row, int((time - (stat(_))[9]) / (24*60*60)));

		if (-f _)
		{	# Regular file, show its size.
			push(@row, right((stat(_))[7] . 'B'));
		} elsif (-d _)
		{	# Directory, give a download link.
			push(@row, right(mklink(
				'Gimme!', 'Download as tarball',
				"$full/$_.tar.gz", 'gimme')));
		} else
		{	# Something else, just leave it empty.
			push(@row, cell(''));
		}

		push(@row, cell(mklink(
				defined $link ? "$_ -> $link" : $_, undef,
				$full)));

		push(@row, cell(exists $$index{$_} ? $$index{$_} : ''));
		push(@list, \@row);
	}

	# Sort by age, format the age column and create the table.
	$list = table(map(row(join('',
			right(color(
				$$_[0] <= 30
					? int(-255/30*$$_[0] + 255)
					: 0,
				0, 0,
				$$_[0] <= 1 ? "$$_[0]day" : "$$_[0]days")),
			@$_[1..$#$_])),
		sort({ $$a[0] <=> $$b[0] } @list)));

	# A little advertisement
	$ad = para("Brought to you by ",
		mklink("Gimme!", 'Get the source', "/$GIMME", "gimmegimme"))
		if !$isrobi;

	# Put all together.
	$client->send_response(HTTP::Response->new(RC_OK, 'Okie',
		[ 'Content-Type' => 'text/html' ],
		html($location, grep(defined,
			($navi, $motd, $desc, $list, $ad)))));
}
# Functionality >>>

# The main loop <<<
# URL-decodes $str.
sub urldecode
{
	my $str = shift;
	$str =~ s/%([0-9][0-9])/{chr(hex($1))}/eg;
	return $str;
}

# Serve $path if we can.
sub serve
{
	my ($c, $r, $path) = @_;

	check_path($c, $path)
		or return;
	if (-f _)
	{
		$c->send_file_response($path->as_string());
	} elsif (-d _)
	{
		send_dir($c, $r, $path);
	} else
	{	# Special file, can't send it.
		print " (special file)";
		$c->send_error(RC_BAD_REQUEST);
	}
}

# Accept connections and serve requests.
sub main
{
	my $d = shift;
	my ($c, $r, $path, $query);

	# Be fair to everyone and allow one request per connection.
	$c = $d->accept('GoodByeClient') until defined $c;
	defined ($r = $c->get_request())
		or return;

	$path = urldecode($r->url()->path());
	print ~~localtime(), " ", $c->peerhost(), ": $path";
	$path = Path->new($path)->as_relative();
	if ($r->method() ne 'GET')
	{	# Filter out junk.
		print " (junk)";
		$c->send_error(RC_METHOD_NOT_ALLOWED);
	} elsif (grep($_ eq '..', @$path))
	{	# Filter out malice.
		print " (forbjuden)";
		$c->send_error(RC_FORBIDDEN);
	} elsif (defined ($query = $r->url()->query()))
	{	# Process special queries.
		print " ($query)";
		if ($query eq 'gimme')
		{
			send_tar($c, $path);
		} elsif ($query eq 'gimmegimme')
		{	# Send GIMME.
			$c->send_response(HTTP::Response->new(
				RC_OK, 'Nesze',
				[ 'Content-Type' => 'text/x-perl' ],
				sub { read_chunk(*GIMME) }));
			seek(GIMME, 0, 0);
		} else
		{	# Ignore $qeury.
			serve($c, $r, $path);
		}
	} else
	{
		serve($c, $r, $path);
	}
	print "\n";

	$c->close();
}
# >>>

# Main starts here
my $d;

unless ($^S)
{	# Initialization, needs to be done only once.
	my ($chroot, $addr, $port, $dir);

	# Parse the command line.
	$addr = '127.0.0.1';
	$port = $> == 0 ? 80 : 8080;
	$dir  = '.';

	@ARGV && $ARGV[0] eq '-C'
		and $chroot = 1
		and shift;

	if (@ARGV == 1)
	{
		-d $ARGV[0] ? $dir : $addr = $ARGV[0];
	} elsif (@ARGV >= 2)
	{
		($addr, $dir) = @ARGV;
	}

	# Keep ourselves open for gimmegimme.
	open(GIMME, '<', $0);

	# Do possibly privileged stuff.
	defined ($d = HTTP::Daemon->new(
			LocalAddr => $addr eq 'all' ? '0.0.0.0' : $addr,
			LocalPort => $port,
			ReuseAddr => 1))
		or die "$!";

	if ($chroot)
	{
		# This will require whatever needs to be by URI.
		URI->new('http://localhost');
		die "$dir: $!" unless chroot($dir);
		chdir(".");
	} else
	{
		die "$dir: $!" unless chdir($dir);
	}

	# Drop privileges.
	if ($> == 0)
	{
		# Change the group membership, the real and effective
		# UIDs and GIDs to the owner of the program so that we
		# can't regain root again.
		my $u = (stat(GIMME))[4];
		($)) = "$u $u";
		($(, $)) = ($u, $u);
		($<, $>) = ($u, $u);
		$( = $u;
		$< = $u;
	}
}

$| = 1;
$SIG{'PIPE'} = 'IGNORE';
$SIG{'HUP'} = sub
{	# Re-execute ourselves.
	no warnings 'redefine';
	local $/ = undef;
	eval <GIMME>;
	seek(GIMME, 0, 0);
	die "gimme decides to die";
};

# Spin the main loop.
eval { main($d) } or not $@ or print $@ until $^S;

# vim: set foldmethod=marker foldmarker=<<<,>>>:
# End of gimme.pl
