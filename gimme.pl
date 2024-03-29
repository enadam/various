#!/usr/bin/perl -w
#
# gimme.pl -- tiny HTTP server for sharing personal stuff <<<
#
# Just start it up and let friends browse and download your public goodies.
# The exported directories will be browseable and downloadable as tarballs.
#
# Synopsis:
#   gimme.pl [--open | --listen <address>] [--port <port>]
#            [--chroot] [--user <user>] [--daemon]
#            [--fork [<max-procs>]] [--max-request-size <bytes>]
#            [--upload | --upload-by-default | --overwrite-by-default]
#            [--auth <user>:<password> | --htaccess]
#            [--nogimme] [<dir>]
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
# With the --htaccess flag you can restrict access of directories selectively
# by dropping .htaccess files there.  This disables tarball generation
# automatically (as if --nogimme was specified), otherwise users would be able
# to circumvent the restrictions by downloading the site.  With the --auth
# option you can protect the entire site without affecting tarball generation.
# If using any of these flags, you should set up stunnel in front of Gimme
# because this authentication is completely cleartext.
#
# The --upload flag enables file uploading with multipart/form-data POST
# requests.  This only enables the functionality, the concrete directories
# where uploading is permitted must be configured so in the .gimme settings
# (see below).  Alternatively the --upload-by-default options can enable
# uploading in every directory where it's not configured explicitly.  The
# --overwrite-by-default flag makes the "upload-enabled overwrite" setting
# the default.  Payloads are first saved to a temporary file, which is removed
# if the upload is unsuccessful.  .dotfiles won't be allowed to be overwritten.
#
# If the --chroot flag is present Gimme chroot()s to <dir>.  This will
# disable directory downloading unless you make tar(1) available in the
# chroot.  Generally speaking, the chrooted mode may be quite fragile
# because it is hard to predict what kind of modules will LWP and the
# others require in run-time.  We're doing our best, though.
#
# Having privileged stuff done Gimme changes its UIDs and GIDs to those
# of <user> if specified, then goes to the background if requested with
# the --daemon flag.  Otherwise access log is written to the standard output.
#
# Gimme was written with public (but not hostile) exposure in mind, so it is
# thought to be relatively secure in the sense that it won't give away stuff
# you didn't intend to share.  However, it can be DoS:ed and generally,
# is not efficient with large files.  To prevent one class of abuse, HTTP
# request sizes are restricted to 10 KiB or 10 MiB if uploading is enabled.
# You can override or disable this limit (by specifying 0 <bytes>) with the
# --max-request-size option.
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
# -- If .htaccess is present in a directory and --htaccess is enabled,
#    Authorization: is demanded from the client for all files and directories
#    beneath.  The format of the file is a single line of "<user>:<password>".
#    If the file is empty, access is denied unconditionally.
# -- .gimme files contain per-directory settings.  This is best illustrated
#    by an example:
#
#    # Comments and empty lines are ignored.
#
#    # Settings in this section apply to all files.  This is also the initial
#    # section (ie. for settings preceding any [section]s).
#    [common]
#    status-code 202
#
#    # Settings in this section apply to all files unless the setting is also
#    # specified in another applicable section.
#    [default]
#    reason-phrase Coming!
#
#    # This section applies to the "alpha" and "beta" files.  These are NOT
#    # regular expressions.
#    [/alpha/, /beta/]
#    header Content-Type text/html
#
#    # This section applies to all files except "gamma" and "delta".
#    [not /gamma/, /delta/]
#    header Foo bar
#    header Foo baz
#
#    # This section applies to the directory where the config is located.
#    # No other section (eg. [common]) applies to this directory.
#    [this-dir]
#    # Uploading files to this directory is possible if the --update flag
#    # is specified.
#    upload-enabled true
#
#    [/epsilon/]
#    # Uploading files to this directory is NOT permitted, even if the
#    # --upload-by-default flag is specified, except if overridden in
#    # [this-dir] of epsilon/.gimme.
#    upload-enabled false
#
#    [/sigma/]
#    # Files (except .dotfiles) can be overwritten in this directory.
#    upload-enabled overwrite
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

sub dirname
{
	my $self = shift;
	return $self->new()->pop()->add(@_);
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
use Carp;
use Errno qw(EACCES);
use HTTP::Status;
use LWP::MediaTypes qw(guess_media_type);

our @ISA = qw(HTTP::Daemon::ClientConn);

# Notify the client we'll close the connection after the response.
sub send_basic_header
{
	my $self = shift;
	$self->SUPER::send_basic_header(@_);
	print $self "Connection: close\r\n";
}

sub send_unauthorized
{
	my ($self, $path) = @_;

	my $rc = RC_UNAUTHORIZED;
	$self->send_basic_header($rc);
	print $self "WWW-Authenticate: Basic realm=", '"', $path, '"', "\r\n";
	print $self "\r\n";
	return $rc;
}

sub send_error_with_warning
{
	my ($self, $what, $error) = @_;

	carp "$what: $!";
	return $self->send_error($error // RC_INTERNAL_SERVER_ERROR);
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

# Send custom headers along with the file response.
sub send_file_response
{
	my ($self, $fname) = @_;
	local *FH;

	if ($$fname[-1] eq ".htaccess")
	{
		return $self->send_error(RC_FORBIDDEN)
	} elsif (!open(FH, '<', $fname))
	{	# check_path() has verified we should have read access,
		# so it's something else.
		return $self->send_error_with_warning($fname);
	}
	binmode(FH);

	if (!$self->antique_client())
	{
		my ($fsize, $mtime);
		my ($config, $headers);

		$config = main::get_per_dir_config($fname->dirname(),
							$$fname[-1]);
		$headers = $$config{'headers'} // HTTP::Headers->new();

		# Set the Content-Length and Last-Modified headers.
		(undef, undef, undef, undef, undef, undef, undef,
			$fsize, undef, $mtime) = stat(FH);
		defined $fsize && defined $mtime
			or warn "$fname: $!";

		$headers->content_length($fsize)
			if defined $fsize;
		$headers->last_modified($mtime)
			if defined $mtime;

		# Guess Content-Type and/or Content-Encoing if they are
		# not included in the $headers.
		if (!defined $headers->content_type()
			|| !defined $headers->content_encoding())
		{
			my ($content_type, $encoding) =
				guess_media_type($fname);
			if (defined $content_type)
			{
				$content_type .= "; charset=UTF8"
					if $content_type eq "text/plain";
				$headers->init_header(
					'Content-Type', $content_type);
			}
			$headers->init_header('Content-Encoding', $encoding)
				if defined $encoding;
		}

		# Send all the $headers.
		$self->send_basic_header(
			$$config{'status-code'}, $$config{'reason-phrase'});
		print $self $headers->as_string();
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

# May be called by send_error() before httpd_client_proto is known.
# Assume the client is not antique initially.
sub antique_client
{
	my $self = shift;

	return defined ${*$self}{'httpd_client_proto'}
		? $self->SUPER::antique_client(@_)
		: 0;
}

sub get_request
{
	my $self = shift;

	my $buf = $self->read_buffer();
	${*$self}{'request_size'} = defined $buf ? length($buf) : 0;
	return $self->SUPER::get_request(@_);
}

sub _need_more
{
	my $self = shift;

	if (defined ${*$self}{'request_size_limit'}
		&& ${*$self}{'request_size'} > ${*$self}{'request_size_limit'})
	{
		warn $self->peerhost(), ": request too large ",
			"(> ${*$self}{'request_size'} B)";
		$self->send_error(RC_PAYLOAD_TOO_LARGE);
		return undef;
	}

	my $n = $self->SUPER::_need_more(@_);
	${*$self}{'request_size'} += $n
		if $n;
	return $n;
}
# >>>

package main;
use strict;
use Getopt::Long;
use File::Temp;
use User::pwent;
use User::grent;
use POSIX qw(setuid setgid uname setsid WNOHANG);
use Fcntl qw(O_CREAT O_WRONLY O_EXCL);
use Errno qw(EEXIST ENOENT ENOTDIR EISDIR EPERM EACCES ENOSPC);
use URI::Escape;
use HTTP::Daemon;
use HTTP::Status;
use HTTP::Headers::Util;

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

# Value of the --auth option.
my $Opt_global_auth;

# Enable .htaccess authorization?
my $Opt_htaccess;

# Display and serve Gimme! links?
my $Opt_gimme;

# Allow uploading files?
my ($Opt_upload_enabled, $Opt_upload_by_default);

# Order of files in a dirlist.
my $Opt_dirlist_order = 'fname';

# Value of the --fork option.
my $Opt_forking;

# Value of the --max-request-size option.
my $Opt_max_request_size;

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
sub form	{ "\t<FORM method=\"POST\" enctype=\"multipart/form-data\">\n"
		. "\t\t<P/>\n@_\t\t<P/>\n\t</FORM>\n" }
sub label	{ "\t\t<LABEL>$_[0]</LABEL>\n"			}
sub file_input	{ "\t\t<INPUT type=\"file\" name=\"$_[0]\">\n"	}
sub text_input	{ "\t\t<INPUT text=\"file\" name=\"$_[0]\">\n"	}
sub submit	{ "\t\t<INPUT type=\"submit\">\n"		}
sub color	{ sprintf('<FONT color="#%.2X%.2X%.2X">%s</FONT>', @_) }

sub mklink
{
	my ($a, $title, $href, $q) = @_;
	my $uri;

	$uri = URI->new();
	$uri->path($href // $a);
	$uri->query($q) if defined $q;

	$a = escape($a);
	if (defined $title)
	{
		$title = escape($title);
		return "<A href=\"$uri\" title=\"$title\">$a</A>";
	} else
	{
		return "<A href=\"$uri\">$a</A>";
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

# Authorize $request based on .htaccess files.
sub check_htaccess
{
	my ($client, $request, $path) = @_;
	my $client_auth;

	# Just return if --htaccess is not enabled.
	$Opt_htaccess
		or return RC_OK;

	# Scan $path from leaf to the current directory for .htaccess files.
	$path = $path->new();
	$client_auth = $request->authorization_basic();
	for (;;)
	{
		my $htaccess;
		local *HTACCESS;

		$htaccess = $path->new(".htaccess");
		if (open(HTACCESS, '<', $htaccess))
		{	# We've found one, $client_auth must satisfy it.
			defined $client_auth
				or return $client->send_unauthorized($path);

			my $auth = <HTACCESS>;
			if (defined $auth)
			{
				chomp($auth);
				last if $client_auth eq $auth;
				return $client->send_unauthorized($path);
			} elsif ($! == 0)
			{	# Empty .htaccess, deny access unconditionally.
				return $client->send_unauthorized($path);
			} else
			{
				return $client->send_error_with_warning(
								$htaccess);
			}
		} elsif ($! != ENOENT)
		{
			return $client->send_error_with_warning($htaccess);
		}

		@$path > 1
			or last;
		$path->pop();
	}

	return RC_OK;
}

sub check_path
{
	my ($client, $path, $error) = @_;

	if (stat($path))
	{
		if (-f _)
		{
			return RC_OK if -r _;
		} elsif (-d _)
		{
			return RC_OK if -r _ && -x _;
		}
		return $client->send_error($error // RC_FORBIDDEN)
	} elsif ($! == ENOENT || $! == ENOTDIR)
	{
		return $client->send_error($error // RC_NOT_FOUND);
	} elsif ($! == EPERM || $! == EACCES)
	{
		return $client->send_error($error // RC_FORBIDDEN);
	} else
	{
		return $client->send_error_with_warning($path,
			$error // RC_INTERNAL_SERVER_ERROR);
	}
}

sub check_dir
{
	my ($client, $request, $path) = @_;
	my $rc;

	($rc = check_path($client, $path)) == RC_OK
		or return $rc;
	return $client->send_error(RC_BAD_REQUEST)
		if ! -d _;
	return check_htaccess($client, $request, $path);
}

# Return whether the user may upload files into $path.
sub may_upload
{
	my $path = shift;
	my $config;

	$Opt_upload_enabled
		or return undef;

	# Is uploading enabled/disabled in [this-dir] of $path/.gimme?
	$config = get_per_dir_config($path);
	return $$config{'upload-enabled'}
		if defined $$config{'upload-enabled'};

	# Is uploading enabled in the parent directory's settings?
	if ($path->dirs())
	{
		$config = get_per_dir_config($path->dirname(), $$path[-1]);
		return $$config{'upload-enabled'}
			if defined $$config{'upload-enabled'};
	}

	return $Opt_upload_by_default;
}

# Take a file in a POST request and save it into $path.
sub upload
{
	my ($client, $request, $path) = @_;
	my ($ok, $rc, $part, $file, $dst, $tmp);

	defined ($ok = may_upload($path))
		or return $client->send_error(RC_FORBIDDEN);
	($rc = check_dir($client, $request, $path)) == RC_OK
		or return $rc;

	# Get the multipart message to upload and the destination file name.
	for $part ($request->parts())
	{
		my (@ary, $disp, %params);

		defined ($disp = $part->header('Content-Disposition'))
			or next;
		(@ary = HTTP::Headers::Util::split_header_words($disp)) > 0
			or next;

		($disp, undef, %params) = @{$ary[0]};
		$disp eq "form-data"
			or next;

		if ($params{'name'} eq "file")
		{
			$file = $part;
		} elsif ($params{'name'} eq "fname")
		{
			$dst = $part->content();
		}
	}

	defined $file
		or return $client->send_error(RC_BAD_REQUEST,
						"Nothing to upload.");

	if (!defined $dst || !length($dst))
	{	# Set a fake header, so the function call won't crash
		# if a file name is not provided with Content-Disposition.
		$file->init_header("Content-Location" => " ");
		$dst = HTTP::Response::filename($file);
	}
	if (defined $dst)
	{	# Sanitize $dst.
		$dst =~ s/^\s+//;
		$dst =~ s/\s+$//;
	}
	if (!defined $dst || !length($dst))
	{
		return $client->send_error(RC_BAD_REQUEST,
						"No file name provided.");
	} elsif ($dst =~ m!/!)
	{	# $dst must be a basename.
		return $client->send_error(RC_BAD_REQUEST);
	}
	print ": $dst";

	# .dotfiles cannot be overwritten.
	$dst !~ /^\./
		or $ok = "no-overwrite";

	# Preliminary check to verify that $dst doesn't exist.
	$dst = $path->new($dst);
	if ($ok ne "overwrite")
	{
		if (lstat($dst))
		{
			$! = EEXIST;
			return $client->send_error(RC_CONFLICT, "$!");
		} elsif ($! != ENOENT)
		{
			return $client->send_error_with_warning($dst);
		}
	}

	# Open a temporary file and write $file to it.  File::Temp must be
	# called in an eval because it croaks (dies) if unsuccessful.
	if (!defined ($tmp = eval {
		File::Temp->new(DIR => $path,
				TEMPLATE => ".gimme.$$dst[-1].XXXXXX") }))
	{
		warn "$@";
		return $client->send_error($! == EACCES
						? RC_FORBIDDEN
						: RC_INTERNAL_SERVER_ERROR);
	} elsif (!$tmp->write(${$file->content_ref()}) || !$tmp->close())
	{
		warn "$tmp: $!";
		return $client->send_error(RC_SERVICE_UNAVAILABLE, "$!")
			if $! == ENOSPC;
		return $client->send_error(RC_INTERNAL_SERVER_ERROR);
	}

	if ($ok eq "overwrite")
	{	# rename(2) overwrites $dst atomically.
		if (!rename($tmp, $dst))
		{
			return $client->send_error(RC_CONFLICT, "$!")
				if $! == EISDIR;
			return $client->send_error(RC_FORBIDDEN)
				if $! == EACCES;
			return $client->send_error_with_warning(
						"rename($tmp -> $dst)");
		}
		$tmp->unlink_on_destroy(0);
	} else
	{	# link(2) doesn't overwrite $dst if it has come into existence
		# since we checked.  $tmp is cleaned up automatically.
		if (!link($tmp, $dst))
		{
			return $client->send_error(RC_CONFLICT, "$!")
				if $! == EEXIST;
			return $client->send_error(RC_FORBIDDEN)
				if $! == EACCES;
			return $client->send_error_with_warning(
						"link($tmp -> $dst)");
		}
	}

	# All OK, redirect the client to wherever it would have gone.
	return serve($client, $request, $path);
}

sub send_tar
{
	my ($client, $request, $path) = @_;
	my $rc;

	# The last component is expected to be the suggested file name.
	$path->pop();
	($rc = check_dir($client, $request, $path)) == RC_OK
		or return $rc;

	return in_subprocess(sub
	{
		local *TAR;
		my ($dir, @tar, $progress);

		# What to transform the initial '.' of the paths in the archive
		# to.  This should be the directory name we made the archive of
		# or $SITE.
		$dir = $path->dirs() ? $$path[-1] : $SITE;
		$dir =~ s/\\/\\\\/g;
		$dir =~ s/!/\\!/g;

		# Make tar(1) transform the path prefixes to $dir.
		@tar = (qw(tar cz), '-C', $path,
			'--transform', "s!^\\.\$!$dir!;s!^\\./!$dir/!",
			'.');

		# Start tar(1) unless we're serving a HEAD request.
		$client->head_request()
			or open(TAR, '-|', @tar)
			or return $client->send_error_with_warning(
					"tar", RC_SERVICE_UNAVAILABLE);

		# Stop spending cycles as soon as the client hangs up.
		my $sigpipe;
		local $SIG{'PIPE'} = sub { $sigpipe = 1 };
		$progress = Progressometer->new($client->peerhost(), $path);
		$client->send_response(HTTP::Response->new(
			RC_OK, 'Here you go',
			[ 'Content-Type' => 'application/x-gtar-compressed' ],
			sub { $sigpipe ? "" : read_chunk(*TAR, $progress) }));

		return RC_OK;
	});
}

# Parse $dir/.gimme and return the config applying to $basename.  If $basename
# is not defined, only return the settings of [this-dir].
sub get_per_dir_config
{
	my ($dir, $basename) = @_;
	my ($confname, %config, %default, $current);
	local *HEADERS;

	$confname = $dir->new(".gimme");
	if (!open(HEADERS, '<', $confname))
	{
		$! == ENOENT
			or warn "$confname: $!";
		return undef;
	}

	# Add the config on the current line to $current, which points to
	# %default if we're processing the [default] section or %config
	# if the current section applies to $basename.
	$current = defined $basename ? \%config : undef;
	$config{'headers'}  = HTTP::Headers->new();
	$default{'headers'} = HTTP::Headers->new();
	while (<HEADERS>)
	{
		next if /^\s*#/ || /^\s*$/;
		chomp;

		# Is this the beginning of a new [section]?
		if (s/^\[(.*)\]$/$1/)
		{
			if (!defined $basename)
			{
				$current = $_ eq "this-dir" ? \%config : undef;
			} elsif ($_ eq "this-dir")
			{	# Skip this section if $basename was provided.
				$current = undef;
			} elsif ($_ eq "common")
			{
				$current = \%config;
			} elsif ($_ eq "default")
			{
				$current = \%default;
			} else
			{
				my $matched = 0;
				my $negative = s/^\s*not\b//;
				while (s!^\s*/(.*?)/\s*,?!!)
				{
					if ($1 eq $basename)
					{
						$matched = 1;
						last;
					}
				}

				#  The section applies if
				#  $matched && !$negative or
				#  !$matched && $negative.
				$current = (!$matched != !$negative)
						? \%config : undef;
			}

			next;
		}

		# Skip setting if section is not applicable.
		defined $current
			or next;

		if (/^(upload-enabled) \s+ (false|true|overwrite)$/x)
		{
			$$current{$1} = $2 eq "false" ? 0 : $2;
		} elsif (/^(status-code|reason-phrase) \s+ (.+)$/x)
		{
			$$current{$1} = $2;
		} elsif (/^header \s+ (\S+) \s+ (.+)$/x)
		{
			$$current{'headers'}->push_header($1, $2);
		} else
		{
			warn "$confname: invalid line";
		}
	}

	# Merge %config with %default.
	defined $config{$_}
		or $config{$_} = $default{$_}
		for grep($_ ne "headers", keys(%default));

	defined $config{'headers'}->header($_)
		or $config{'headers'}->header(
			$_ => [ $default{'headers'}->header($_) ])
		for $default{'headers'}->header_field_names();

	return \%config;
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
	my ($navi, $motd, $desc, $upload, $ad);
	my ($index, @list, $list);
	local *MOTD;

	# Is $client a robot?
	$isrobi = defined ($ua = $request->header('User-Agent'))
		&& $ua =~ $ROBOTS;

	# Upper navigation bar
	$location = Path->new('/');
	if (!$isrobi)
	{
		my (@upper, @lower);

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
		$index = get_index($path->dirname("00INDEX"));
		$desc = $$index{$$path[-1]};
		$desc = defined $desc ? para($desc) : '';
	} else
	{
		$desc = para("This is ${SITE}'s root directory.");
	}

	# The upload form.
	$upload = form(label("Upload file:"), file_input("file"),
			label("File name:"), text_input("fname"),
			submit())
		if may_upload($path);

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
		$Opt_dirlist_order eq "age"
			? $lhs <=> $rhs
			: $lhs cmp $rhs;
	} @list)));

	# A little advertisement.
	$ad = para("Brought to you by",
		mklink("Gimme!", 'Get the source', "/$GIMME", "gimmegimme"))
		unless !$Opt_gimme || $isrobi;

	# Put it all together.
	$client->send_response(HTTP::Response->new(RC_OK, 'Okie',
		[ 'Content-Type' => 'text/html; charset=UTF8' ],
		html($location, grep(defined,
			($navi, $motd, $desc, $upload, $list, $ad)))));

	return RC_OK;
}
# Functionality >>>

# The main loop <<<
# Serve $path if we can.
sub serve
{
	my ($c, $r, $path) = @_;
	my $rc;

	if (($rc = check_path($c, $path)) != RC_OK)
	{
		return $rc;
	} elsif (-f _)
	{
		($rc = check_htaccess($c, $r, $path->dirname())) == RC_OK
			or return $rc;
		return $c->send_file_response($path);
	} elsif (-d _)
	{
		my $dirlist;

		($rc = check_htaccess($c, $r, $path)) == RC_OK
			or return $rc;

		# Is there a .dirlist file in $path?
		if (!lstat($dirlist = $path->new(".dirlist")))
		{	# No, except if there was an error.
			return $c->send_error_with_warning($dirlist)
				if $! != ENOENT;
			return send_dir($c, $r, $path);
		} elsif (($rc = check_path($c, $dirlist,
					RC_INTERNAL_SERVER_ERROR)) != RC_OK)
		{	# It's an internal error if $dirlist doesn't check out.
			return $rc;
		} elsif (-f _)
		{
			return $c->send_file_response($dirlist);
		} elsif (-d _)
		{	# $dirlist must not point to another directory.
			$! = EISDIR;
			return $c->send_error_with_warning($dirlist);
		}
	}

	# Special file, should have been caught by check_path().
	return $c->send_error(RC_INTERNAL_SERVER_ERROR);
}

# Accept connections and serve requests.
sub main
{
	my $d = shift;
	my ($c, $r, $path, $auth, $query, $rc);
	my @warnings;

	# Be fair to everyone and allow one request per connection.
	$c = $d->accept('GoodByeClient') until defined $c;
	$c->timeout(60);
	${*$c}{'request_size_limit'} = $Opt_max_request_size;
	defined ($r = $c->get_request())
		or return;

	# Buffer warnings until the end of request processing,
	# when we can print them clearly.
	local $SIG{'__WARN__'} = sub { push(@warnings, $_[0]) };

	$path = URI::Escape::uri_unescape($r->url()->path());
	print ~~localtime(), " ", $c->peerhost(),
		": ", $r->method(), ": ", $path;
	$path = Path->new($path)->as_relative();
	if (!grep($r->method() eq $_, "HEAD", "GET", "POST"))
	{	# Filter out junk.
		$rc = $c->send_error(RC_METHOD_NOT_ALLOWED);
	} elsif (grep($_ eq '..', @$path))
	{	# Filter out malice.
		$rc = $c->send_error(RC_FORBIDDEN);
	} elsif (defined $Opt_global_auth
		&& (!defined ($auth = $r->authorization_basic())
			|| $auth ne $Opt_global_auth))
	{
		$rc = $c->send_unauthorized('/');
	} elsif ($r->method() eq "POST")
	{
		$rc = upload($c, $r, $path);
	} elsif (defined ($query = $r->url()->query()))
	{	# Process special queries.
		print " ($query)";
		if ($Opt_gimme && $query eq 'gimme')
		{
			$rc = send_tar($c, $r, $path);
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
sub drop_privileges
{
	my ($user, @groups) = @_;

	$! = 0;
	$) = join(' ', $user->gid(), map($_->gid(), @groups));
	die "setgroups(", join(', ', map($_->name(), @groups)), "): $!"
		if $!;

	setgid($user->gid())
		or die "setgid(", $user->gid(), "): $!";
	setuid($user->uid())
		or die "setuid(", $user->name(), "): $!";
}

sub get_user
{
	my $user = shift;
	my ($pwent, $grent, @groups);

	defined ($pwent = getpwnam($user))
		or die "$user: unknown user";

	# Collect the group memberships of $user.
	while (defined ($grent = getgrent()))
	{	# Add $grent to @groups if $user is in $grent->members().
		push(@groups, $grent)
		        if grep($_ eq $pwent->name(), @{$grent->members()});

	}
	endgrent();

	return ($pwent, @groups);
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
	my ($addr, $port, $user, $chroot, $daemonize, $dir);
	my (@groups, $d);

	# Parse the command line.
	$addr = '127.0.0.1';
	$port = $> == 0 ? 80 : 8080;
	Getopt::Long::Configure(qw(gnu_getopt));
	exit(1) unless GetOptions(
		'a|open'		=> sub { undef $addr },
		'i|listen=s'		=> \$addr,
		'p|port=i'		=> \$port,
		'u|user=s'		=> \$user,
		'C|chroot!'		=> \$chroot,
		'D|daemon!'		=> \$daemonize,
		'f|fork:i'		=> \$Opt_forking,
		'max-request-size=i'	=> \$Opt_max_request_size,
		'auth=s'		=> \$Opt_global_auth,
		'htaccess!'		=> \$Opt_htaccess,
		'gimme!'		=> \$Opt_gimme,
		'upload!'		=> \$Opt_upload_enabled,
		'upload-by-default'	=> sub
		{
			$Opt_upload_enabled = 1;
			$Opt_upload_by_default = 1;
		},
		'overwrite-by-default'	=> sub
		{
			$Opt_upload_enabled = 1;
			$Opt_upload_by_default = "overwrite";
		},
		'sort-by-name'		=> sub
		{
			$Opt_dirlist_order = 'fname';
		},
		'sort-by-age'		=> sub
		{
			$Opt_dirlist_order = 'age';
		},
	);

	# Set a default $Opt_max_request_size.
	if (!defined $Opt_max_request_size)
	{
		$Opt_max_request_size = $Opt_upload_enabled
						? 10*1024*1024 : 10*1024;
	} elsif (!$Opt_max_request_size)
	{	# Unlimited
		undef $Opt_max_request_size;
	}

	# In my tests user agents didn't send multiple Authorization headers,
	# so it's pointless to have both.
	die "--htaccess doesn't go with --auth"
		if defined $Opt_global_auth && $Opt_htaccess;

	# Disable Gimme! by default if --htaccess is enabled.
	defined $Opt_gimme
		or $Opt_gimme = !$Opt_htaccess;

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

	# get_user() before chrooting.
	($user, @groups) = get_user($user)
		if defined $user;

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

	drop_privileges($user, @groups)
		if defined $user;

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
