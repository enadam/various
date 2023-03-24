#!/usr/bin/perl -w
#
# jelsaw.pl -- simple password manager with OAuth 2.0 support
#
# Secrets are taken from INI files and the chosen one is copied to the primary
# X or console selection, from which you can paste it in another window or
# terminal with the middle button of the mouse.  If an INI file is encrypted
# with GnuPG, it is decyrpted automatically.
#
# The vaults are searched under the directory specified by $JELSAW_VAULT
# and its subdirectories or ~/.config/jelsaw by default (you can symlink
# it to anywhere else).
#
# To run the Config::IniFiles perl module (libconfig-inifiles-perl in Debian),
# GnuPG and xsel(1) must be installed.  The program supports both X and the
# text console (via gpm or consolation).
#
# The interactive mode needs Term::ReadLine, and the OAuth functionality
# deoends on LWP and HTTP::Daemon (except in out-of-band mode).
#
# Usage: <<<
# jelsaw.pl [--vaults|-V <vaults>] [--newline|-n] [--timeout|-t <seconds>]
#           <vault> [<section>/][<key>]
#   Copy the secret value of <key> ("password" by default) from one of the
#   <vault>'s <section>s (the "default" one if unspecified).  <vault> is
#   either a direct path to an INI file or it's taken as the case-insensitive
#   prefix of a file name.  In the latter case the <vault> is searched in
#   <vaults> (repeat the option to specify more than one) or the locations
#   described above.  You have <seconds> time (5 by default) to paste the
#   secret value.  After that the program exits and the secret is deleted from
#   the selection.  You can finish earlier by pressing <Enter>.  -t 0 disables
#   the timeout.  By default the secret value is pasted without a terminating
#   newline.  Specify --newline to add one.
#
# jelsaw.pl --find|-f <vault>
#   Return the vault that woule be opened.
#
# jelsaw.pl --edit|-w <vault>
#   Edit the <vault> with your $VISUAL editor.
#
# jelsaw.pl --all|-a <vault>
#   Show the <vault> as-is in plaintext.
#
# jelsaw.pl --view|-v [<section>]/ <vault>
#   Show <section>'s all keys and secret values.
#
# jelsaw.pl --view|-v [<section>/]<key> <vault>
#   Show the specified <key>'s secret value.
#
# jelsaw.pl --copy|-c [<section>/][<key>] [--timeout|-t <seconds>] <vault>
#   Copy the specified <key>'s secret value.
#   Same as jelsaw.pl <vault> [<section>/][<key>].
#
# jelsaw.pl --oauth2 --view|--copy [<section>/][refresh|access] <vault>
#   Obtain a refresh or access token from an OAuth 2.0 provider and print it
#   or copy it to the clipboard.  See below for details.
#
# jelsaw.pl --overview|-l <vault>
#   Show the sections and keys in the <vault>, but not the secret values.
#
# jelsaw.pl --interactive|-i [<vault>]
#   If specified, open the <vault>, then prompt for your commands to see
#   or copy secrets.  Enter '?' to see the options.
# >>>
#
# A <vault> file (for example "shops.gpg") could be: <<<
#
# # This is the default section.
# username = me@mine.org
# password = hihihi
#
# [site1]
# password = ha#ha#ha # This is a comment.
# security question = Who are you?
# answer = Dunno \# Last time I knew.
#
# [site2]
# security question = What do you want?
# answer = Everything
#
# Examples:
#
# $ jelsaw.pl shops
#   Copies "hihihi" from the default section.
#
# $ jelsaw.pl shops username
#   Copies "me@mine.org".
#
# $ jelsaw.pl shops site1/
#   Copies "ha#ha#ha".
#
# $ jelsaw.pl shops site1/answer
#   Copies "Dunno # Last time I knew.".
#
# $ jelsaw.pl shops -v username
#   Prints "me@mine.org".
#
# $ jelsaw.pl shops -v /
#   Prints:
# username = me@mine.org
# password = hihihi
#
# $ jelsaw.pl shops -v site1/
#   Prints:
# security question = Who are you?
# answer            = Dunno
#
# $ jelsaw.pl shops -l
#   Prints:
# username
# password
#
# [site1]
# security question
# answer
#
# [site2]
# security question
# answer
# >>>
#
# OAuth 2.0 support <<<
# ---------------------
#
# If the necessary keys are present in a section, jelsaw can obtain a refresh
# or access token from an OAuth provider (inspired by
# https://github.com/google/gmail-oauth2-tools/blob/master/python/oauth2.py).
# An access token can be used for IMAP or SMTP authentication for example.
#
# Suppose you have these in your vault:
#
# [gmail]
# oauth2_provider	= https://accounts.google.com
# oauth2_scope		= https://mail.google.com/
#			# Only needed for refresh token generation.
# oauth2_client_id	= ...
# oauth2_client_secret	= ...
# oauth2_refresh_token	= ...	# Needed for access token generation.
#
# $ jelsaw.pl --oauth2 <vault> gmail/access
#
# Will go to the oauth2_provider and exchange oauth2_refresh_token for a
# short-lived access token.  "access" can be omitted, as that's the default.
# If the settings are in the default section, you can omit the argument
# altogether.
#
# $ jelsaw.pl --oauth2 <vault> gmail/refresh
#
# Prints an URL for you to open.  It will redirect your browser to your OAuth
# provider, which will authenticate you, confirm the access reqeust, and again
# redirect you to the webserver started by jelsaw.  It will then extract the
# authorization code passed along by the OAuth provider, exchange it for a
# refresh token and copy it to the clipboard.  From there you can add it to
# the vault.
#
# $ jelsaw.pl --oauth2 <vault> gmail/refresh_oob
#
# Like above, but executes the workflow in out-of-band mode.  In this case no
# webserver is started (so the HTTP::Daemon module needn't be installed) and
# you will be asked to copy the authorization code displayed by the OAuth
# provider and enter it at the prompt.  This workflow is intended as fallback
# if the other doesn't work, and may be unsupported by your provider (eg. see
# https://developers.googleblog.com/2022/02/making-oauth-flows-safer.html#disallowed-oob).
#
# See this guide how to set up an oauth2_client_id and secret:
# https://gitlab.com/muttmua/mutt/-/blob/master/contrib/mutt_oauth2.py.README
# Note that with GCP (Google) the refresh token will expire in 7 days unless
# you "publish" your "app".  However, before publication you don't have to
# submit it for verification (as long as you're the only one using it I guess).
# >>>

# Modules
use strict;
use POSIX;
use IO::File;
use Getopt::Long;
use Config::IniFiles;

# Constants
use constant TIOCL_SETSEL	=> 2;
use constant TIOCL_SELCHAR	=> 0;

my $CLRSCR			= "\e[1;1H" . "\e[2J";
my $CSRPOS_REQUEST		= "\e[6n";
my $CSRPOS_REPLY		= qr/^\e\[(\d+);(\d+)R$/;
my $COLOR_INVISIBLE		= "\e[30;40m";
my $COLOR_DEFAULT		= "\e[39;49m";

my @DFLT_VAULTS = ("$ENV{'HOME'}/.config/jelsaw");
my @GPG = qw(gpg --batch --quiet --decrypt --);
my @XSEL = qw(xsel --input --logfile /dev/null --nodetach);
my $OAUTH2_REDIR = "urn:ietf:wg:oauth:2.0:oob";

# Private variables
my ($Opt_newline, $Opt_timeout);

# Program code
# Strip the end of a "die" message.
sub die2msg
{
	my $msg = shift;
	$msg =~ s/ at \Q$0\E line \d+\b.*\n$//;
	return $msg;
}

# Print the usage on STDOUT or STDERR if we're having an $error.
sub usage
{
	my $error = shift;

	defined $error
		or $error = 1;
	my $fh = $error ? \*STDERR : \*STDOUT;

	my $me = $0;
	$me =~ s!^.*/+!!;

	print $fh "INI+GPG-based password manager.";
	print $fh "";
	print $fh "Usage:";
	print $fh "  $me <vault> [[<section>]/][<key>]";
	print $fh "  $me --find|-f <vault>";
	print $fh "  $me --edit|-w <vault>";
	print $fh "  $me --all|-a <vault>";
	print $fh "  $me --view|-v|--copy|-c [<section>]/ <vault>";
	print $fh "  $me --view|-v|--copy|-c [[<section>]/]<key> <vault>";
	print $fh "  $me --copy|-c <section-and/or-key> <vault>";
	print $fh "  $me --overview|-l <vault>";
	print $fh "  $me --interactive|-i [<vault>]";
	print $fh "";
	print $fh "Optional parameters:";
	print $fh "  --vaults|-V <dir>";
	print $fh "  --newline|-n";
	print $fh "  --timeout|-t <seconds>";
	print $fh "";
      	print $fh "See $0 for the complete documentation.";

	exit($error);
}

# Find a file matching $prefix in one of the @_ directories or their
# subdirectories.
sub find_vault
{
	my $prefix = shift;
	my ($is_absolute, @dirs);
	my ($pattern, @prefix_matches, @full_matches);

	# Allow opening a vault by direct path.
	return $prefix if -e $prefix;

	@dirs = @_;
	$is_absolute = $prefix =~ s!^/+!!;
	if ($is_absolute && $prefix =~ s!(.*)/+!!)
	{	# Only search in dirname($prefix).
		$_ = "$_/$1" for @dirs;

	}
	$pattern = qr!^.*/\Q$prefix\E(.*)\.(?:ini|gpg)$!i;

	# Search all subdirectories of @dirs (except if $is_absolute).
	while (@dirs)
	{
		my ($dir, $fname);

		$dir = shift(@dirs);
		for $fname (<$dir/*>)
		{
			unshift(@dirs, $fname)
				if !$is_absolute && -d $fname;
			next if ! -f _;

			my ($postfix) = $fname =~ $pattern;
			if ($postfix)
			{
				push(@prefix_matches, $fname);
			} elsif (defined $postfix)
			{
				push(@full_matches, $fname);
			}
		}
	}

	if (@full_matches > 1)
	{
		die "More than one $prefix found.";
	} elsif (@full_matches == 1)
	{
		return $full_matches[0];
	} elsif (@prefix_matches > 1)
	{
		die "More than one $prefix found.";
	} elsif (@prefix_matches == 1)
	{
		return $prefix_matches[0];
	} else
	{
		die "$prefix not found.";
	}
}

# Return whether a vault should be decrypted when opening it.
sub is_encrypted
{
	$_[0] =~ /\.gpg$/
}

# Decrypt the contents of $vault and parse it as an INI.
sub load_vault
{
	my $vault = shift;
	my $ini;

	if (is_encrypted($vault))
	{
		open(GPG, '-|', @GPG, $vault)
			or die "$GPG[0]: $!";
		$ini = Config::IniFiles->new(-file => \*GPG, -fallback => "default");
		close(GPG) # Wait until @GPG finishes to see if there was an error.
			or return undef;
	} else
	{
		$ini = Config::IniFiles->new(-file => $vault, -fallback => "default");
	}

	if (!defined $ini)
	{	# Syntax error in the $vault.
		local $, = "\n";
		print STDERR "$vault";
		print STDERR @Config::IniFiles::errors;
		return undef;
	} else
	{
		return $ini;
	}
}

# Extract the sections and keys from $ini for the purpose of
# tab-completion.
sub extract_completions
{
	my ($ini, $completions) = @_;

	@$completions = ();
	for my $section ($ini->Sections())
	{
		push(@$completions, $section);
		foreach my $key ($ini->Parameters($section))
		{
			push(@$completions, $key)
				if $section eq "default";
			push(@$completions, "$section/$key")
		}
	}

	# Escape \-es and spaces to make tab-completion work on words
	# containing escaped spaces and backslashes.
	s/( |\\)/\\$1/g
		for @$completions;
}

# Load the $vault and set the global references to it.
sub open_vault
{
	my ($vault, $rini, $completions) = @_;
	my $ini;

	if (defined ($ini = load_vault($vault)))
	{
		$$rini = $ini;
		extract_completions($ini, $completions);
		return 1;
	} else
	{
		return 0;
	}
}

# Die if $ini is not open.
sub check_vault
{
	my $ini = shift;

	die "No vault is open."
		if !defined $ini;
}

# Print $ini's sections and keys.
sub overview_cmd
{
	my $ini = shift;

	for my $section ($ini->Sections())
	{
		print $section, ":";
		print "  ", $_
			foreach $ini->Parameters($section);
	}
}

# Parse a command parameter as "section/key".
sub parse_section_and_key
{
	my ($which, $dflt_key) = @_;
	my ($section, $key);

	if (ref $which)
	{	# Already decomposed.
		($section, $key) = @$which;
	} elsif ($which =~ m!^(.*)/(.*)$!)
	{	# $which is "$section/$key", but either of them can be empty.
		($section, $key) = ($1, $2);
	} else
	{
		$key = $which;
	}

	# Empty value -> default.
	return ($section || "default", $key || $dflt_key);
}

# Return the value of $section/$key in $ini.
sub retrieve_key
{
	my ($ini, $section, $key) = @_;
	my $val;

	check_vault($ini);
	if (!defined ($val = $ini->val($section, $key)))
	{
		die "$section/$key: no such key";
	} else
	{
		return $val;
	}
}

# Print a key's value or an entire section of $ini.
sub view_cmd
{
	my ($ini, $what) = @_;
	my ($section, $key);

	($section, $key) = parse_section_and_key($what);
	if (defined $key)
	{
		print retrieve_key($ini, $section, $key);
	} elsif (!$ini->SectionExists($section))
	{
		die "$section: no such section";
	} else
	{	# Show the whole $section.
		my (@keys, $maxwidth);

		# Align the key values.
		@keys = $ini->Parameters($section);
		defined $maxwidth && $maxwidth > length($_)
				or $maxwidth = length($_)
			foreach @keys;
		printf("%-*s = %s\n", $maxwidth, $_, $ini->val($section, $_))
			foreach @keys;
	}
}

# Delete trailing comment from a key's value.
sub uncomment
{
	my $str = shift;

	$str =~ s/\s+#\s+.*$//;
	$str =~ s/\\#/#/g;

	return $str;
}

# Wait until the user hits <Enter> or $Opt_timeout elapses.
sub wait_for_enter
{
	if ($Opt_timeout)
	{
		$SIG{'ALRM'} = sub { die };
		alarm($Opt_timeout);
	}

	eval
	{
		local $SIG{'__DIE__'};
		readline;
		alarm(0);
	};
}

# Determine the cursor position on $tty.
sub cursor_position
{
	my $tty = shift;
	my ($term, $prev_lflag, $n, $pos);

	# We'll need to print $CSRPOS_REQUEST on $tty.  Turn off canonical
	# input processing and echo to make it work and invisible to the user.
	$term = POSIX::Termios->new();
	defined $term->getattr($tty->fileno())
		or die "tcgetattr(\"/dev/tty\"): $!";
	$prev_lflag = $term->getlflag();
	$term->setlflag($prev_lflag & ~(POSIX::ICANON|POSIX::ECHO));
	defined $term->setattr($tty->fileno(), POSIX::TCSANOW)
		or die "tcsetattr(\"/dev/tty\"): $!";

	# Request the cursor position.
	$tty->print($CSRPOS_REQUEST);

	# Wait for the reply.
	for (;;)
	{
		my $rfd;

		vec($rfd, $tty->fileno(), 1) = 1;
		if (($n = select($rfd, undef, undef, undef)) < 0)
		{
			die "select(\"/dev/tty\"): $!";
		} elsif ($n > 0)
		{
			last;
		}
	}

	# Restore the terminal attributes.
	$term->setlflag($prev_lflag);
	defined $term->setattr($tty->fileno(), POSIX::TCSANOW)
		or die "tcsetattr(\"/dev/tty\"): $!";

	# Determine the length of the reply.
	$n = pack("L", 0);
	defined $tty->ioctl(FIONREAD(), $n)
		or die "ioctl(\"/dev/tty\", FIONREAD): $!";
	$n = unpack("L", $n);

	# Read and parse the reply.
	defined $tty->read($pos, $n)
		or die "/dev/tty: $!";
	$pos =~ $CSRPOS_REPLY
		or die "/dev/tty: couldn't determine current cursor position";
	return ($1, $2);
}

# Return the number of rows and columns of the terminal.
sub terminal_size
{
	my $tty = shift;
	my ($template, $struct, $rows, $cols);

	$template = "S!4";
	$struct = pack($template);
	defined $tty->ioctl(TIOCGWINSZ(), $struct)
		or die "ioctl(\"/dev/tty\", TIOCGWINSZ): $!";
	($rows, $cols) = unpack($template, $struct);

	return ($rows, $cols);
}

# Copy the contents of the currently active virtual console from ($x1, $y1)
# to ($x2, $y2) (inclusive).
sub set_console_selection
{
	my $tty = shift;
	my ($x1, $y1, $x2, $y2) = @_;

	my $struct = pack("c S!5", TIOCL_SETSEL,
				$x1, $y1, $x2, $y2, TIOCL_SELCHAR);
	defined $tty->ioctl(TIOCLINUX(), $struct)
		or die "ioctl(\"/dev/tty\", TIOCLINUX, TIOCL_SETSEL): $!";
}

# Copy $str to the console selection, so it can be pasted by gpm.
sub copy_to_console_selection
{
	my $str = shift;
	my ($tty, $width, $height, $x, $y);
	my $full_line;

	# Import the ioctl() constants we'll use.
	do
	{	# There are some warnings while importing this module,
		# suppress them.
		local $^W = 0;
		require "sys/ioctl.ph";
	};

	# The standard input and output could be redirectred to somewhere,
	# Open the controlling terminal we can use to operate on.
	defined ($tty = IO::File->new("/dev/tty", "r+"))
		or die "/dev/tty: $!";
	$tty->autoflush(1);
	local $\ = "";

	# Print $str on the top-left of the screen in black on black
	# background, so it's not visible even for a fraction of a second.
	$tty->print($CLRSCR, $COLOR_INVISIBLE, $str, $COLOR_DEFAULT);
	($y, $x) = cursor_position($tty);
	($height, $width) = terminal_size($tty);

	# Printing something on the screeen automatically advances the cursor,
	# except if the string's last character happens to land on the last
	# column of the terminal.  $full_line tries to capture this condition.
	# If false, we assume there's a trailing whitespaece, which would add
	# a newline to the selection when pasting.  Depending on $Opt_newline
	# this may or may not be what we want.  If not, we need to update the
	# end coordinates.  This algorithm is fuzzy at best.
	$full_line = ($x == $width && length($str) % $width == 0);
	if ($Opt_newline)
	{	# The user wants to paste the selection with a newline.
		if ($full_line)
		{	# We need to add a newline, but can't if we have a
			# screenful of text; don't try to be smart in this
			# case.
			if ($y < $height)
			{
				$tty->print("\n");
				$y++;
				$x = $width;
			}
		} elsif ($x > 1)
		{	# There's a trailing whitespace, which will be pasted
			# as newline.
		} elsif ($y > 1)
		{	# $str already ends in a newline.
			$y--;
			$x = $width;
		} elsif ($str eq "")
		{	# Copy a single empty line.
			$x = $width;
		} else
		{	# $str must include control characters, and driven the
			# cursor back to the top-left corner.  Copy the whole
			# screen, we can't do anything more sensible with it.
			($y, $x) = ($height, $width);
		}
	} else
	{	# Remove trailing whitespace.
		if ($full_line)
		{	# Nothing to remove.
		} elsif ($x > 1)
		{	# The normal case.
			$x--;
		} elsif ($y > 1)
		{	# $str must be ending in a newline.
			$y--;
			$x = $width;
		} elsif ($str eq "")
		{	# Copy a space, we can't do less.
		} else
		{	# See above.
			($y, $x) = ($height, $width);
		}
	}

	# Copy the contents of the screen from the top-left corner, then clear
	# the screen at once to make $str irrecoverable except via pasting.
	set_console_selection($tty, 1, 1, $x, $y);
	$tty->print($CLRSCR, "Go!\n");
	wait_for_enter();
	$tty->print($CLRSCR);
	set_console_selection($tty, 1, 1, 1, 1);
}

# Pass $str to xsel(1) to copy it to the primary selection.
sub copy_to_x11_selection
{
	my $str = shift;
	my $xsel;
	local *XSEL;

	$str .= "\n" if $Opt_newline && $str !~ /\n$/;;

	defined ($xsel = open(XSEL, "|-", @XSEL))
		or die "$XSEL[0]: $!";

	# $xsel reads STDIN until EOF, so we must close the input pipe.
	# We can't just close() it because that waits until $xsel exits.
	select(XSEL);
	local $\ = "";
	local $| = 1;
	print XSEL $str;
	select(STDOUT);
	POSIX::close(fileno(XSEL));

	print "Go!\n";
	wait_for_enter();
	kill(TERM => $xsel);
}

# Copy a string to the appropriate selection.
sub copy_to_selection
{
	exists $ENV{'DISPLAY'}
		? copy_to_x11_selection(@_)
		: copy_to_console_selection(@_);
}

# Copy a key from $ini to the clipboard.
sub copy_cmd
{
	my ($ini, $what) = @_;

	copy_to_selection(
		uncomment(retrieve_key(
			$ini, parse_section_and_key($what, "password"))));
}

# Unescape "\ " and "\\".
sub unescape
{
	my $str = shift;
	$str =~ s/\\( |\\)/$1/g;
	return $str;
}

# Import all the modules required for the oauth2_*() functions.
# Since this functioanality is optional, we import them on demand.
sub init_oauth2
{
	require URI;
	require URI::QueryParam;
	require LWP::UserAgent;
	require HTTP::Request::Common;
	require JSON::PP;
}

# Acquire a new refresh_token from an OAuth provider.
sub oauth2_new_refresh_token
{
	my ($ini, $oob, $section, $cmd) = @_;
	my ($provider, $client_id, $client_secret, $scope);
	my ($auth_request_url, $http_client, $auth_code);
	my ($http_reply, $json);

	$provider	= retrieve_key($ini, $section, "oauth2_provider");
	$client_id	= retrieve_key($ini, $section, "oauth2_client_id");
	$client_secret	= retrieve_key($ini, $section, "oauth2_client_secret");
	$scope		= retrieve_key($ini, $section, "oauth2_scope");

	# First send the user to get the $auth_code from the $provider.
	$auth_request_url = URI->new("$provider/o/oauth2/auth");
	$auth_request_url->query_form(
		client_id	=> $client_id,
		scope		=> $scope,
		response_type	=> "code");

	if ($oob)
	{	# Copy-paste.
		$auth_request_url->query_param(redirect_uri => $OAUTH2_REDIR);
		print $auth_request_url;
		do
		{
			local $\ = "";
			print "Visit the URL above and enter the ",
				"authorization code: ";
		};
		defined ($auth_code = readline(*STDIN))
			or die "Cancelled.";
		chomp($auth_code);
	} else
	{	# Start a webserver.
		require HTTP::Daemon;
		require HTTP::Status;
		my ($server, $server_cookie);
		my ($start_url, $finish_url, $url);

		do
		{	# Define a custom HTTP::Daemon::ClientConn,
			# which works regarless of the value of $\.
			package MyClient;

			our @ISA = qw(HTTP::Daemon::ClientConn);

			sub send_error
			{
				local $\ = "";
				shift->SUPER::send_error(@_);
			}

			sub send_redirect
			{
				local $\ = "";
				shift->SUPER::send_redirect(@_);
			}

			sub send_response
			{
				local $\ = "";
				shift->SUPER::send_response(@_);
			}
		};

		# Ignore requests not carrying the $server_cookie.
		# This protects against leaking the $auth_request_url.
		$server_cookie = int(rand(0xFFFFFFFF));
		defined ($server = HTTP::Daemon->new(LocalAddr => '127.0.0.1'))
			or die "$!.";

		$start_url = URI->new($server->url());
		$start_url->query_form(cookie => $server_cookie);
		$start_url->path("start");
		print "Please open this URL: ", $start_url;

		$finish_url = $start_url->clone();
		$finish_url->path("finish");

		$auth_request_url->query_param(redirect_uri => $finish_url);

		# The webserver loop.  It redirects the client to the
		# $auth_request_url and finishes when the client requests the
		# $finish_url via a redirect by the authentication provider.
		for (;;)
		{
			my ($request, $client_cookie);

			defined ($http_client = $server->accept("MyClient"))
				or next;
			defined ($request = $http_client->get_request())
				or next;

			if ($request->method() ne "GET")
			{
				$http_client->send_error(
					HTTP::Status::HTTP_METHOD_NOT_ALLOWED());
				next;
			}

			# Verify the $client_cookie.
			$url = $request->uri();
			$client_cookie = $url->query_param('cookie');
			if (!defined $client_cookie
				|| $client_cookie ne $server_cookie)
			{
				$http_client->send_error(
					HTTP::Status::HTTP_UNAUTHORIZED());
				next;
			}

			if ($url->path() eq $start_url->path())
			{
				$http_client->send_redirect($auth_request_url);
			} elsif ($url->path() eq $finish_url->path())
			{
				last;
			} else
			{
				$http_client->send_error(
					HTTP::Status::HTTP_FORBIDDEN());
			}
		}

		if (!defined ($auth_code = $url->query_param("code")))
		{
			$http_client->send_error(
				HTTP::Status::HTTP_BAD_GATEWAY());
			die "$provider: no authorization code returned";
		}
	}

	# Exchange the $auth_code for a refresh_token.
	$http_reply = LWP::UserAgent->new()->request(
		HTTP::Request::Common::POST("$provider/o/oauth2/token",
		[
			client_id	=> $client_id,
			client_secret	=> $client_secret,
			code		=> $auth_code,
			grant_type	=> "authorization_code",
			redirect_uri	=> $auth_request_url->query_param(
						"redirect_uri"),
		]));
	if (!$http_reply->is_success())
	{
		$http_client->send_response($http_reply)
			if defined $http_client;
		die "$provider: ", $http_reply->status_line();
	}

	$json = JSON::PP::decode_json($http_reply->content());
	if (!defined $$json{"refresh_token"})
	{
		$http_client->send_error(HTTP::Status::HTTP_BAD_GATEWAY())
			if defined $http_client;
		die "$provider: 'refresh_token' missing";
	}

	if (defined $http_client)
	{
		$http_client->send_response(HTTP::Response->new(
			HTTP::Status::HTTP_OK(), "OK", undef,
			"Success!  You can close this window now."));
	}

	if ($cmd eq "view")
	{	# If the access_token is missing, that's odd, but not a
		# fatal error.
		print "Refresh token: ", $$json{"refresh_token"};
		print "Access token:  ", $$json{"access_token"}
			if defined $$json{"access_token"};
	} else
	{
		copy_to_selection($$json{"refresh_token"});
	}
}

# Acquire a new access_token from a refresh_token.
sub oauth2_refresh_access_token
{
	my ($ini, $section, $cmd) = @_;
	my ($provider, $client_id, $client_secret, $refresh_token);
	my ($http_reply, $json);

	$provider	= retrieve_key($ini, $section, "oauth2_provider");
	$client_id	= retrieve_key($ini, $section, "oauth2_client_id");
	$client_secret	= retrieve_key($ini, $section, "oauth2_client_secret");
	$refresh_token	= retrieve_key($ini, $section, "oauth2_refresh_token");

	$http_reply = LWP::UserAgent->new()->request(
		HTTP::Request::Common::POST("$provider/o/oauth2/token",
		[
			client_id	=> $client_id,
			client_secret	=> $client_secret,
			refresh_token	=> $refresh_token,
			grant_type	=> "refresh_token",
		]));
	$http_reply->is_success()
		or die "$provider: ", $http_reply->status_line();

	$json = JSON::PP::decode_json($http_reply->content());
	defined $$json{"access_token"}
		or die "$provider: 'access_token' missing";

	if ($cmd eq "view")
	{
		print $$json{"access_token"};
	} else
	{
		copy_to_selection($$json{"access_token"});
	}
}

# Main starts here.
my (@opt_vaults, $opt_find);
my ($opt_edit, $opt_view_all, $opt_overview, $opt_interactive);
my ($opt_view, $opt_copy, $opt_oauth2);
my (@modes, $vault, $ini);

$\ = "\n";
$SIG{'__DIE__'} = sub
{
	print STDERR die2msg(shift);
	exit 1;
};

# Parse the command line.
$Opt_timeout = 5;
Getopt::Long::Configure(qw(gnu_getopt));
exit(1) unless GetOptions(
	'h|help'	=> sub { usage(0) },
	'V|vaults=s'	=> \@opt_vaults,
	'f|find'	=> \$opt_find,
	'w|edit'	=> \$opt_edit,
	'a|all'		=> \$opt_view_all,
	'v|view=s'	=> \$opt_view,
	'c|copy=s'	=> \$opt_copy,
	'A|oauth2'	=> \$opt_oauth2,
	'n|newline!'	=> \$Opt_newline,
	't|timeout=i'	=> \$Opt_timeout,
	'l|overview'	=> \$opt_overview,
	'i|interactive'	=> \$opt_interactive,
);

# Set @opt_vaults if it wasn't specified on the command line.
if (!@opt_vaults)
{
	if (defined $ENV{'JELSAW_VAULT'})
	{
		@opt_vaults = ($ENV{'JELSAW_VAULT'});
	} else
	{
		@opt_vaults = @DFLT_VAULTS;
	}
}

# Find the $vault to read the secrets from.
if (@ARGV > 0)
{
	$vault = find_vault(shift, @opt_vaults);
} elsif (!$opt_interactive)
{
	usage(0);
}

@modes = grep(defined $_,
	($opt_find, $opt_edit,
		$opt_view_all, $opt_view, $opt_copy,
		$opt_overview, $opt_interactive));
if (@ARGV)
{	# $ARGV[0] is what to copy to the clipboard.
	usage() if @modes;
	usage() if @ARGV > 1;
	$opt_copy = shift;
} elsif (!@modes)
{	# Copy the default password by default.
	$opt_copy = [ ];
} elsif (@modes > 1)
{	# More than one mode is selected.
	usage();
}

if ($opt_oauth2)
{
	init_oauth2();
	defined $opt_view || defined $opt_copy
		or die "--oauth2 only makes sense with the view or copy ",
			"command";
}

if ($opt_find)
{
	print $vault;
	exit;
} elsif ($opt_edit)
{	# Edit $vault.
	exec($ENV{'VISUAL'}, $vault);
} elsif ($opt_view_all)
{	# Show the whole $vault in cleartext?
	exec(is_encrypted($vault) ? @GPG : qw(cat), $vault)
		or die "$GPG[0]: $!";
} elsif (defined $vault && !defined ($ini = load_vault($vault)))
{
	exit 1;
}

if ($opt_overview)
{	# Just show the sections and the keys, but not the vaules.
	overview_cmd($ini);
} elsif ($opt_interactive)
{	# Let the user pick what to view or copy interactively.
	my ($term, @completions);

	if (-t STDIN)
	{	# Only load readline-support in interactive mode.
		require Term::ReadLine;

		$term = Term::ReadLine->new('jelsaw');
		$term->ornaments(0);

		# Set up tab-completion of sections and keys.
		extract_completions($ini, \@completions)
			if defined $ini;

		# The first word can be a command.
		my @commands = qw(
			open reopen edit
			reveal view copy
			newline timeout
			gen-oauth2-refresh-token
			gen-oauth2-refresh-token-oob
			gen-and-copy-oauth2-refresh-token
			gen-and-copy-oauth2-refresh-token-oob
			gen-oauth2-access-token
			gen-and-copy-oauth2-access-token);
		$term->Attribs->{'completion_function'} = sub
		{
			my ($text, $line, $start) = @_;
			substr($line, 0, $start) =~ /^\s*$/
				? (@completions, @commands)
				: @completions;
		};

		# A space is quoted if it's preceded by a \, but not \\.
		$term->Attribs->{'char_is_quoted_p'} = sub
		{
			my ($line, $index) = @_;
			return reverse(substr($line, 0, $index+1)) =~
				/^ \\(?:\\\\)*(?!\\)/;
		};

		$term->Attribs->{'completer_word_break_characters'} =~ s/\\//;
		$term->Attribs->{'completer_quote_characters'} = "\\";
	}

	# The main loop.
	for (;;)
	{
		# Exit silently if the user hit ^D.
		my $what = defined $term ? $term->readline("? ") : <STDIN>;
		defined $what
			or last;

		# Strip whitespace.
		$what =~ s/^\s+//;
		$what =~ s/\s+$//;

		if (defined $term)
		{	# Add the stripped input to the history or replace it
			# with the previous command.
			$term->remove_history($term->where_history())
				if $term->Features->{'autohistory'};
			if ($what ne "!!")
			{
				$term->add_history($what);
			} elsif (!defined ($what = $term->history_get(
							$term->where_history())))
			{
				print STDERR "No previous command.";
				next;
			}
		}

		my $ret = eval
		{
			local $SIG{'__DIE__'} = undef;
			local $| = 1 unless -t STDOUT;
			if ($what eq "exit" || $what eq "q")
			{
				return 0;
			} elsif ($what eq "help" || $what eq '?')
			{	# <<<
				print "help, ?                  - ",
					"???";
				print "exit, q, ^D              - ",
					"I'm done";
				print "!!                       - ",
					"do it again";
				print "[re]open [<vault>]       - ",
					"reload the current vault or open ",
					"another one";
				print "edit                     - ",
					"edit the vault file and reload it";
				print "<Enter>                  - ",
					"see an overview of the vault";
				print "reveal                   - ",
					"show the entire vault";
				print "view [<section>/]        - ",
					"view the selected <section> ",
					"or the default one";
				print "view [<section>/]<key>   - ",
					"view the selected <key>";
				print "[copy] [<section>/]<key> - ",
					"copy the selected <key> ",
					"to clipboard";
				print "copy [<section>/]        - ",
					"copy <section>/password or ",
					"default/password";
				print "newline                  - ",
					"toggle --newline";
				print "timeout <seconds>        - ",
					"change --timeout";
				print "";
				print "gen-oauth2-refresh-token",
					' ' x 16, "[<section>]";
				print "    Acquire and print a new refresh ",
					"token from an OAuth provider.";
				print "gen-oauth2-refresh-token-oob",
					' ' x 12, "[<section>]";
				print "    Acquire a new refresh token in ",
					"out-of-band mode (with manual ",
					"copy-paste).";
				print "gen-oauth2-access-token",
					' ' x 17, "[<section>]";
				print "    Acquire and print a new access ",
					"token.";
				print "gen-and-copy-oauth2-refresh-token",
					"[-oob] [<section>]";
				print "    Acquire a new refresh token and ",
					"copy it to the clipboard.";
				print "gen-and-copy-oauth2-access-token",
					' ' x 8, "[<section>]";
				print "    Acquire a new access token and ",
					"copy it to the clipboard.";
				# >>>
			} elsif ($what eq "")
			{
				check_vault($ini);
				overview_cmd($ini);
			} elsif ($what =~ s/^(?:re)?open(?:\s+|$)//)
			{	# Open a new $vault or reload the current one.
				if (!$what)
				{
					check_vault($ini);
					open_vault($vault, \$ini, \@completions);
				} else
				{
					my $new_vault = find_vault(
						unescape($what), @opt_vaults);
					open_vault($new_vault, \$ini,
							\@completions)
						and $vault = $new_vault;
				}
			} elsif ($what eq "edit")
			{	# If the edition was successful, reload $ini.
				check_vault($ini);
				open_vault($vault, \$ini, \@completions)
					if system($ENV{'VISUAL'}, $vault) == 0;
			} elsif ($what eq "reveal")
			{
				local $\ = "";
				check_vault($ini);
				$ini->OutputConfig();
			} elsif ($what eq "newline")
			{
				$Opt_newline = !$Opt_newline;
				if ($Opt_newline)
				{
					print "Add newline to selection.";
				} else
				{
					print "Do not add a newline to the ",
						"selection.";
				}
			} elsif ($what =~ s/^timeout(?:\s+|$)//)
			{
				$what =~ /^\d+$/
					or die "Usage: timeout <seconds>";
				$Opt_timeout = int($what);
			} elsif ($what =~ s/^gen(-and-copy)?-oauth2
						-refresh-token(-oob)?
						(?:\s+|$)//x)
			{
				init_oauth2();
				oauth2_new_refresh_token($ini, defined $2,
					$what ? unescape($what) : "default",
					defined $1 ? "copy" : "view");
			} elsif ($what =~ s/^gen(-and-copy)?-oauth2
						-access-token(?:\s+|$)//x)
			{
				init_oauth2();
				oauth2_refresh_access_token($ini,
					$what ? unescape($what) : "default",
					defined $1 ? "copy" : "view");
			} elsif ($what =~ s/^view(?:\s+|$)//)
			{
				view_cmd($ini, unescape($what));
			} else
			{
				$what =~ s/^copy(?:\s+|$)//;
				copy_cmd($ini, unescape($what));
			}
			return 1;
		};

		if ($@)
		{
			print STDERR die2msg($@);
		} elsif (!$ret)
		{
			last;
		}
	}
} elsif ($opt_oauth2)
{
	my ($cmd, $section, $key);

	$cmd = defined $opt_view ? "view" : "copy";
	($section, $key) = parse_section_and_key(
		$opt_view || $opt_copy, "access_token");
	if ($key =~ /^refresh(?:_token)?(_oob)?$/)
	{
		oauth2_new_refresh_token($ini, defined $1, $section, $cmd);
	} elsif ($key eq "access" || $key eq "access_token")
	{
		oauth2_refresh_access_token($ini, $section, $cmd);
	} else
	{
		die "$key: should be either \"refresh\" or \"access\"";
	}
} elsif (defined $opt_view)
{
	view_cmd($ini, $opt_view);
} else
{
	copy_cmd($ini, $opt_copy);
}

# vim: set foldmethod=marker foldmarker=<<<,>>>:
# End of jelsaw.pl
