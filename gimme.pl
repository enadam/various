#!/usr/bin/perl -w
#
# gimme.pl -- tiny HTTP server for sharing personal stuff <<<
#
# Just start it up and let friends browse and download your public goodies.
# The exported directories will be browseable and downloadable as tarballs.
#
# Synopsis:
#   gimme.pl [--open|--listen <address>] [--port <port>] [--chroot]
#            [--daemon] [--fork [<max-procs>]] [--nogimme] [<dir>]
#
# <address> is the one to listen on, defaulting to localhost.  Use --open
# to make the service available on all interfaces.  By default the port
# is 80 if started as root, otherwise it's 8080.
#
# <dir> designates the wwwroot of the service.  Clients will be able to get
# everything underneath, but nothing outside.  Symlink targets are treated
# as if they were within wwwroot.  For example if a symlink points to /tmp
# clients will have access to it and its children, but not its parent (/).
# Non-regular files (sockets, FIFO:s and device nodes) are not served.
# If not specified otherwise <dir> will be the current working directory.
#
# Unless --fork is specified files and tarballs are served by a single
# process, blocking subsequent requests.  Otherwise a new process is started
# until <max-procs> is reached, then requests are served sequentially.
# If <max-procs> is omitted or 0 no such limit is imposed.
#
# If the --chroot flag is present Gimme chroot()s to <dir>.  This will
# disable directory downloading unless you make tar(1) available in the
# chroot.  Generally speaking, the chrooted mode may be quite fragile
# because it is hard to predict what kind of modules will LWP and the
# others require in run-time.  We're doing our best, though.
#
# Having privileged stuff done Gimme changes its UIDs and GIDs to those
# owning the executable, then goes to the background if requested with
# the --daemon flag.  Otherwise access log is written to the standard output.
#
# Gimme was written with public (but not hostile) exposure in mind, so it is
# thought to be relatively secure in the sense that it won't give away stuff
# you didn't intend to share.  However, it can be DoS:ed and generally,
# is not efficient with large files.
#
# Special files:
# -- If the requested path is a directory and there is not a .dirlist
#    file therein, the directory's contents are listed, sorted according to
#    the --sort-by-* flags.  Unless disabled with --nogimme, there will be
#    "Gimme!" links for directories that let clients download them in a
#    single .tar archive.  (This capability was the primary motivation
#    for this program.) When creating a tarball symlinks are not followed.
#    These and other navigation links are not presented to robots.
# -- If a .dirlist file is present in the directory, its contents are
#    returned as the directory listing.
# -- .dotfiles are not shown in directory listings, but they remain
#    accessible by addressing them directly, so they are still public.
# -- 00INDEX is a series of <file-name> <description> mappings, each
#    line describing a file in the directory.  These descriptions are
#    shown in directory listings.
# -- <dir>/.motd is displayed in every directory listing.
# -- If .htaccess is present in a directory Authorization: is demanded
#    from the client (but _not_ verified).
# -- .headers: specify or override HTTP headers (e.g. Content-Type)
#    when sending files from that directory.  You can supply a custom
#    status line with the X-Status-Code and X-Reason-Phrase headers.
#    You can define "[common]" and "[default]" headers, or restrict
#    them to apply to specific "[/files/, /files2/]" or the other way
#    around ("[not /file1/, /file2/]").
# >>>

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

	if (ref $path)
	{
		return bless([ @$path ],  ref $class || $class);
	} elsif (defined $path && $path ne '')
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

# Download progress indicator.
package Progressometer; # <<<
use strict;

# Log completion percentages at this intervals.
my $CHECKPOINT_SECS = 5;

sub new
{
	my ($class, $peer, $fname, $fsize) = @_;
	return bless({
		peer		=> $peer,
		fname		=> $fname,
		fsize		=> $fsize,
		read		=> 0,
		checkpointed	=> undef,
		next_checkpoint	=> time() + $CHECKPOINT_SECS,
	}, ref $class || $class);
}

# Tell the Progressometer that $read bytes have been or is about to be sent
# to the client or that the end of stream has been reached, in which case a
# final update is made.
sub update
{
	my ($this, $read) = @_;

	$$this{'read'} += $read
		if defined $read;

	# Show the progress if it's an intermediate update and enough time
	# has elapsed since the last one or if it's a final update which is
	# different from the previous one.
	my $now = time();
	if (defined $read
		? $now >= $$this{'next_checkpoint'}
		: (!defined $$this{'checkpointed'}
			|| $$this{'checkpointed'} < $$this{'read'}))
	{
		my $progress;

		# The progress is either displayed as a percentage if the full
		# expected size is known or as a byte counter.
		if ($$this{'fsize'})
		{
			$progress = sprintf('%d%%',
				int(100 * $$this{'read'} / $$this{'fsize'}));
		} elsif (defined $$this{'fsize'} && !$$this{'read'})
		{	# Read 0 bytes of 0.
			$progress = "100%";
		} else
		{
			$progress = main::humanize_size($$this{'read'})
						// "$$this{'read'} B";
		}

		if (!defined $SIG{'__WARN__'})
		{	# We're in a subprocess, log a complete line.
			print	~~localtime($now), " ",
				$$this{'peer'}, ": ", $$this{'fname'}, ": ",
				$progress, "\n";
		} else
		{
			print defined $$this{'checkpointed'}
				? ", " : ": ", $progress;
		}

		$$this{'checkpointed'} = $$this{'read'};
		$$this{'next_checkpoint'} = $now + $CHECKPOINT_SECS;
	}
}
# >>>

# Override HTTP::Daemon::ClientConn.
package GoodByeClient; # <<<

use strict;
use Errno qw(EACCES);
use HTTP::Status;
use HTTP::Date qw(time2str);
use LWP::MediaTypes qw(guess_media_type);

our @ISA = qw(HTTP::Daemon::ClientConn);

# Notify the client we'll close the connection after the response.
sub send_basic_header
{
	my $self = shift;
	$self->SUPER::send_basic_header(@_);
	print $self "Connection: close\r\n";
}

sub send_file
{
	my ($self, $fh, $fname) = @_;

	my $progress = Progressometer->new($self->peerhost(), $fname,
						(stat($fh))[7]);
	for (;;)
	{
		my ($n, $buf);

		# Read and forward $fh.
		if (!defined ($n = read($fh, $buf, 8*1024)))
		{
			warn "$fname: $!";
			return;
		} elsif ($n == 0)
		{	# End of file.
			$progress->update();
			last;
		} elsif (!print $self $buf)
		{
			warn $self->peerhost(), ": ", "$!";
			return;
		} else
		{
			$progress->update($n);
		}
	}
}

# Send custom .headers along with the file response.
sub send_file_response
{
	my ($self, $fname) = @_;
	my ($headers, $basename);
	local *FH;

	$headers = Path->new($fname)->pop()->add(".headers");
	$headers = $headers->as_string();
	$basename = $$fname[-1];
	$fname = $fname->as_string();

	if (!open(FH, '<', $fname))
	{	# check_path() has verified we should have read access,
		# so it's something else.
		warn "$fname: $!";
		return $self->send_error(RC_INTERNAL_SERVER_ERROR);
	}
	binmode(FH);

	if (!$self->antique_client())
	{
		my ($fsize, $mtime);
		my (@status_line, @headers, %headers);
		my ($content_type, $encoding);
		local *HEADERS;

		# Get $fname's $fsize and $mtime.
		(undef, undef, undef, undef, undef, undef, undef,
			$fsize, undef, $mtime) = stat(FH);
		defined $fsize && defined $mtime
			or warn "$fname: $!";

		# HEADERS => @status_line, @headers
		my $section = "[common]";
		if (open(HEADERS, '<', $headers))
		{
			LINE:
			while (<HEADERS>)
			{
				my ($new_section, $header);

				# Is it the beginning of a new $section?
				chomp;
				if ($_ eq "[common]" || $_ eq "[default]")
				{
					$section = $_;
					next;
				} elsif (s/^\[(.*)\]$/$1/)
				{
					my $negative = s/^\s*not\b//;
					while (s!^\s*/(.*?)/\s*,?!!)
					{
						if ($1 ne $basename)
						{
							next;
						} elsif ($negative)
						{	# Negative match.
							undef $section;
							next LINE;
						} else
						{	# Positive match.
							$section = "positive";
							next LINE;
						}
					}

					$section = $negative
						? "negative" : undef;
					next;
				} elsif (!defined $section)
				{	# This section is not applicable.
					next;
				}

				# Don't set @status_line if we're in the
				# [default] section and it's defined already.
				if (s/^X-Status-Code:\s*//i)
				{
					$section eq "[default]"
						&& defined $status_line[0]
						or $status_line[0] = $_;
					next;
				} elsif (s/^X-Reason-Phrase:\s*//i)
				{
					$section eq "[default]"
						&& defined $status_line[1]
						or $status_line[1] = $_;
					next;
				}

				($header) = $_ =~ /^([^:]+)\s*:/;
				defined $header
					or next;
				$header = lc($header);

				if ($section ne "[default]"
					|| !$headers{$header})
				{
					push(@headers, $_);
					$headers{$header} = 1;
				}
			}
			close(HEADERS);
		}

		# Add an OK status code if we only got the reason phrase.
		!@status_line || defined $status_line[0]
			or $status_line[0] = RC_OK;

		# Guess Content-Type and/or Content-Encoing if they are
		# not included in the %headers.
		if (!$headers{'content-type'}
			&& !$headers{'content-encoding'})
		{
			($content_type, $encoding) = guess_media_type($fname);
		} elsif (!$headers{'content-type'})
		{
			$content_type = guess_media_type($fname);
			$encoding = undef;
		} elsif (!$headers{'content-encoding'})
		{
			$content_type = undef;
			(undef, $encoding) = guess_media_type($fname);
		} else
		{
			$content_type = $encoding = undef;
		}

		# Send all the headers.
		$self->send_basic_header(@status_line);
		print $self "Content-Type: $content_type\r\n"
			if defined $content_type;
		print $self "Content-Encoding: $encoding\r\n"
			if defined $encoding;
		print $self "Content-Length: $fsize\r\n"
			if defined $fsize;
		print $self "Last-Modified: ", time2str($mtime), "\r\n"
			if defined $mtime;
		print $self $_, "\r\n"
			foreach @headers;

		print $self "\r\n";
	}

	return $self->head_request()
		? RC_OK
		: main::in_subprocess(sub
		{
			$self->send_file(\*FH, $fname);
			return RC_OK;
		});
}
# >>>

package main;
use strict;
use Getopt::Long;
use Scalar::Util;
use POSIX qw(uname setsid WNOHANG);
use Errno qw(ENOENT ENOTDIR EISDIR EPERM EACCES);
use HTTP::Daemon;
use HTTP::Status;

# For chrooted mode.
use File::Basename;
use LWP::MediaTypes;

# Default file name for 'gimmegimme'.
my $GIMME = $0;
$GIMME =~ s!^.*/+!!;

# Default file name of the .tgz of the site.
my (undef, $SITE) = uname();
chomp($SITE);

# Don't tire these user agents with navigation bar or advertisement.
my $ROBOTS = qr/\b(?:wget|googlebot)\b/i;

# Display and serve Gimme! links?
my $Opt_gimme = 1;

# Order of files in a dirlist.
my $Opt_dirlist_order = 'fname';

# Value of the --fork option.
my $Opt_forking;

# PID of processess fork()ed by in_subprocess().  Used to determine
# whether we have enough quota for another child.
my %Children;

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

# Subprocess management <<<
# If conditions permit fork() a subprocess and execute $fun() within it.
# Otherwise just call the function.
sub in_subprocess
{
	my $fun = shift;
	my $child;

	if (defined $Opt_forking
		&& (!$Opt_forking || keys(%Children) < $Opt_forking))
	{	# We're a forking server and we have free quota.
		if (!defined ($child = fork()))
		{
			warn "fork(): $!";
		} elsif ($child)
		{	# Parent, register $child.
			$Children{$child} = 1;
		} else
		{	# Child, ignore SIGCHLD because we don't need
			# child_exited() to manage the grand-%Children.
			$SIG{'CHLD'} = "IGNORE";
		}
	}

	if (!defined $child)
	{	# We didn't fork().
		return $fun->();
	} elsif (!$child)
	{	# We're in the subprocess.
		undef $SIG{'__WARN__'};
		$fun->();
		exit 0;
	} else
	{	# We're the parent.
		return RC_OK;
	}
}

# The SIGCHLD handler if $Opt_forking.
sub child_exited
{
	my $pid;

	# Reap all finished subprocesses.
	delete $Children{$pid}
		while defined ($pid = waitpid(-1, WNOHANG)) && $pid > 0;
}
# Subprocesses >>>

# Functionality <<<
sub read_chunk
{
	my ($fh, $progress) = @_;
	my $buf;

	if (!defined read($fh, $buf, 4096))
	{
		warn "tar: $!";
		return undef;
	} else
	{	# If $buf is empty we've reached the end of file.
		$progress->update(length($buf) || undef)
			if defined $progress;
		return $buf;
	}
}

sub check_path
{
	my ($client, $path) = @_;

	if (stat($path))
	{
		if (-f _)
		{
			return RC_OK if -r _;
		} elsif (-d _)
		{
			return RC_OK if -r _ && -x _;
		}
		return $client->send_error(RC_FORBIDDEN)
	} elsif ($! == ENOENT || $! == ENOTDIR)
	{
		return $client->send_error(RC_NOT_FOUND);
	} elsif ($! == EPERM || $! == EACCES)
	{
		return $client->send_error(RC_FORBIDDEN);
	} else
	{
		warn "$path: $!";
		return $client->send_error(RC_INTERNAL_SERVER_ERROR);
	}
}

sub send_tar
{
	my ($client, $path) = @_;
	my $rc;

	# The last component is expected to be the suggested file name.
	$path->pop();
	($rc = check_path($client, $path)) == RC_OK
		or return $rc;
	return $client->send_error(RC_BAD_REQUEST)
		if ! -d _;

	return in_subprocess(sub
	{
		my ($dir, $progress);
		local *TAR;

		# What to transform the initial '.' of the paths in the archive
		# to.  This should be the directory name we made the archive of
		# or $SITE.
		$dir = $path->dirs() ? $$path[-1] : $SITE;
		$dir =~ s/\\/\\\\/g;
		$dir =~ s/!/\\!/g;

		# Start tar(1) and make it transform the path prefixes to $dir
		# if we are serving the body of the response.
		if (!$client->head_request() && !open(TAR, '-|',
			qw(tar cz), '-C', $path,
			'--transform', "s!^\\.\$!$dir!;s!^\\./!$dir/!", '.'))
		{
			warn "tar: $!";
			return $client->send_error(RC_SERVICE_UNAVAILABLE);
		}

		# Stop spending cycles as soon as the client hangs up.
		my $sigpipe;
		local $SIG{'PIPE'} = sub { $sigpipe = 1 };
		$progress = Progressometer->new($client->peerhost(), $path);
		$client->send_response(HTTP::Response->new(
			RC_OK, 'Here you go',
			[ 'Content-Type' => 'application/x-tar' ],
			sub { $sigpipe ? "" : read_chunk(*TAR, $progress) }));

		return RC_OK;
	});
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

# Return $size as "$size XiB" or undef if $size is less than a KiB.
sub humanize_size
{
	my $size = shift;

	for my $prefix (qw(B KiB MiB GiB TiB PiB EiB))
	{
		if ($size >= 1024)
		{
			$size /= 1024;
		} elsif ($prefix eq 'B')
		{
			return undef;
		} else
		{
			return sprintf('%.2f %s', $size, $prefix);
		}
	}
}

sub send_dir
{
	my ($client, $request, $path) = @_;
	my ($ua, $isrobi, $location);
	my ($motd, $navi, @upper, @lower);
	my (@list, $list, $ad);
	my ($index, $desc);
	local *MOTD;

	# Is $client a robot?
	$isrobi = defined ($ua = $request->header('User-Agent'))
		&& $ua =~ $ROBOTS;

	# Upper navigation bar
	$location = Path->new('/');
	if (!$isrobi)
	{
		push(@upper, mklink($location, 'Site root'));
		push(@lower, mklink('Gimme!', 'Download site',
					"/$SITE.tar.gz", 'gimme'))
			if $Opt_gimme;

		my @dirs = $path->dirs();
		while (@dirs)
		{
			my $dir = shift(@dirs);
			$location->add($dir);
			push(@upper, mklink($dir,
				@dirs > 0
					? 'Go to upper directory'
					: 'This directory',
				$location));
			push(@lower, mklink('Gimme!',
				@dirs > 0
					? 'Download tree'
					: 'Download this directory',
					"$location/$dir.tar.gz", 'gimme'))
				if $Opt_gimme;
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
		my ($link, $entry);

		$link = readlink($_);
		stat($_)
			or defined $link && lstat($_)
			or next;

		s!^.*/+!!;
		push(@list, $entry =
		{
			fname	=> $_,
			symlink	=> $link,
			age	=> int((time - (stat(_))[9]) / (24*60*60)),
	       	});

		if (-f _)
		{
			$$entry{'size'} = (stat(_))[7];
		} elsif (-d _)
		{
			$$entry{'is_dir'} = 1;
		}
	}

	# Sort by $Opt_dirlist_order, format the columns and create the table.
	$list = table(map({
		my @row;

		my $age = $$_{'age'};
		my $red = $age <= 30 ? int(-255/30*$age + 255) : 0;
		push(@row, right(color(($red, 0, 0),
				$age <= 1 ? "${age}day" : "${age}days")));

		# Add the size column or download link.
		my $fname = $$_{'fname'};
		my $full = $location->new($fname);
		if (defined $$_{'size'})
		{
			my $size = $$_{'size'};
			my $human = humanize_size($size);
			push(@row, defined $human
				? (right($human), right("($size B)"))
				: (right("$size B"), cell('')));
		} elsif ($$_{'is_dir'})
		{
			push(@row, cell(''));
			if (!$Opt_gimme || $isrobi)
			{
				push(@row, right(escape('<DIR>')));
			} else
			{
				push(@row, right(mklink(
					'Gimme!', 'Download as tarball',
					"$full/$fname.tar.gz", 'gimme')));
			}
		} else
		{	# Neither a file or a directory.
			push(@row, cell(''), cell(''));
		}

		my $link = $$_{'symlink'};
		push(@row, cell(mklink(
				defined $link ? "$fname -> $link" : $fname,
				undef, $full)));
		push(@row, cell($$index{$fname} // ''));

		row(join('', @row));
	} sort({
		my $lhs = $$a{$Opt_dirlist_order};
		my $rhs = $$b{$Opt_dirlist_order};
		Scalar::Util::looks_like_number($lhs)
			? $lhs <=> $rhs
			: $lhs cmp $rhs;
	} @list)));

	# A little advertisement.
	$ad = para("Brought to you by",
		mklink("Gimme!", 'Get the source', "/$GIMME", "gimmegimme"))
		unless !$Opt_gimme || $isrobi;

	# Put it all together.
	$client->send_response(HTTP::Response->new(RC_OK, 'Okie',
		[ 'Content-Type' => 'text/html' ],
		html($location, grep(defined,
			($navi, $motd, $desc, $list, $ad)))));

	return RC_OK;
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
	my ($dirlist, $rc);

	my $authorization = $r->header("Authorization");
	if (!defined $authorization)
	{
		my $dir = Path->new($path)->pop();
		if (-f $dir->add(".htaccess")->as_string())
		{
			$rc = RC_UNAUTHORIZED;
			$c->send_basic_header($rc);
			print $c "WWW-Authenticate: Basic realm=",
				'"', $dir->as_string(), '"', "\r\n";
			print $c "\r\n";
			return $rc;
		}
	} else
	{
		print " (Authorization: $authorization)"
	}

	# Is there a .dirlist file in $path?
	lstat($dirlist = $path->new()->add(".dirlist"));
	if (-e _)
	{	# It must not point to another directory.
		if (-d $dirlist)
		{
			$! = EISDIR;
			warn "$dirlist: $!";
			return $c->send_error(RC_INTERNAL_SERVER_ERROR);
		}
		$path = $dirlist;
	}

	if (($rc = check_path($c, $path)) != RC_OK)
	{
		return $rc;
	} elsif (-f _)
	{
		return $c->send_file_response($path);
	} elsif (-d _)
	{
		return send_dir($c, $r, $path);
	} else
	{	# Special file, should have been caught by check_path().
		return $c->send_error(RC_INTERNAL_SERVER_ERROR);
	}
}

# Accept connections and serve requests.
sub main
{
	my $d = shift;
	my ($c, $r, $path, $query, $rc);
	my @warnings;

	# Be fair to everyone and allow one request per connection.
	$c = $d->accept('GoodByeClient') until defined $c;
	defined ($r = $c->get_request())
		or return;

	# Buffer warnings until the end of request processing,
	# when we can print them clearly.
	local $SIG{'__WARN__'} = sub { push(@warnings, $_[0]) };

	$path = urldecode($r->url()->path());
	print ~~localtime(), " ", $c->peerhost(),
		": ", $r->method(), ": ", $path;
	$path = Path->new($path)->as_relative();
	if (!grep($r->method() eq $_, "HEAD", "GET"))
	{	# Filter out junk.
		$rc = $c->send_error(RC_METHOD_NOT_ALLOWED);
	} elsif (grep($_ eq '..', @$path))
	{	# Filter out malice.
		$rc = $c->send_error(RC_FORBIDDEN);
	} elsif (defined ($query = $r->url()->query()))
	{	# Process special queries.
		print " ($query)";
		if ($Opt_gimme && $query eq 'gimme')
		{
			$rc = send_tar($c, $path);
		} elsif ($Opt_gimme && $query eq 'gimmegimme')
		{	# Send GIMME.
			$c->send_response(HTTP::Response->new(
				RC_OK, 'Nesze',
				[ 'Content-Type' => 'text/x-perl' ],
				sub { read_chunk(*GIMME) }));
			seek(GIMME, 0, 0);
			$rc = RC_OK;
		} else
		{	# Ignore $query.
			$rc = serve($c, $r, $path);
		}
	} else
	{
		$rc = serve($c, $r, $path);
	}
	$c->close();

	# Log the fate of the request if not successful.
	print " (", HTTP::Status::status_message($rc), ")"
		if $rc != RC_OK;
	print "\n";

	# The logline is complete now, we can log the warnings.
	print STDERR "  ", $_
		for @warnings;
}
# >>>

# Initialization <<<
# Change the group membership, the real and effective UIDs and GIDs
# to the owner of the program so that we can't regain root again.
sub drop_privileges
{
	my $u = (stat(GIMME))[4];
	($)) = "$u $u";
	($(, $)) = ($u, $u);
	($<, $>) = ($u, $u);
	$( = $u;
	$< = $u;
}

# Go to the background by forking a grandchild.  Inspired by Proc::Daemon.
sub daemonize
{
	my $pid;
	local (*CHLD_IN, *CHLD_OUT);

	# The grandchild will let the parent know through this pipe
	# if it hatched successfully.
	pipe(CHLD_IN, CHLD_OUT)
		or die "pipe(): $!";

	if (!defined ($pid = fork()))
	{
		die "fork(): $!";
	} elsif ($pid)
	{	# Parent process.  If the pipe is closed without anything
		# being written to it, there was a problem.
		close(CHLD_OUT);
		defined ($pid = readline(CHLD_IN))
			or exit 1;
		print "Gimme has gone to the background as $pid";
		exit;
	} else
	{	# Child process.  Detach from the parent's session.
		close(CHLD_IN);
		setsid() >= 0
			or die "setsid(): $!";

		if (!defined ($pid = fork()))
		{
			die "fork(): $!";
		} elsif ($pid)
		{	# The first child has no more duties.
			exit;
		}

		# The grandchild process.
		open(STDIN, '<', "/dev/null")
			or die "/dev/null: $!";
		open(STDOUT, '>', "/dev/null")
			or die "/dev/null: $!"
			if -t STDOUT;
		open(STDERR, '>', "/dev/null")
			or die "/dev/null: $!"
			if -t STDERR;

		print CHLD_OUT "$$\n";
		close(CHLD_OUT);
	}
}

# Initialize the program.
sub init
{
	my ($addr, $port, $chroot, $daemonize, $dir);
	my $d;

	# Parse the command line.
	$addr = '127.0.0.1';
	$port = $> == 0 ? 80 : 8080;
	Getopt::Long::Configure(qw(gnu_getopt));
	exit(1) unless GetOptions(
		'a|open'		=> sub { undef $addr },
		'i|listen=s'		=> \$addr,
		'p|port=i'		=> \$port,
		'C|chroot!'		=> \$chroot,
		'D|daemon!'		=> \$daemonize,
		'f|fork:i'		=> \$Opt_forking,
		'gimme!'		=> \$Opt_gimme,
		'sort-by-name'		=> sub
		{
			$Opt_dirlist_order = 'fname';
		},
		'sort-by-age'		=> sub
		{
			$Opt_dirlist_order = 'age';
		},
	);

	# Determine the directory to serve.
	if (!@ARGV)
	{
		$dir = '.';
	} elsif (@ARGV == 1)
	{
		$dir = shift(@ARGV);
	} else
	{
		die "Too many arguments";
	}

	# Keep ourselves open for gimmegimme.
	open(GIMME, '<', $0);

	# Do possibly privileged stuff.
	defined ($d = HTTP::Daemon->new(
			LocalAddr => $addr // '0.0.0.0',
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

	# Drop privileges if started as root.
	drop_privileges
		if $> == 0;

	daemonize()
		if $daemonize;

	$| = 1;
	$SIG{'PIPE'} = 'IGNORE';
	$SIG{'CHLD'} = \&child_exited
		if defined $Opt_forking;

	return $d;
}
# >>>

# Spin the main loop.
my $d = init();
main($d) while 1;

# vim: set foldmethod=marker foldmarker=<<<,>>>:
# End of gimme.pl
