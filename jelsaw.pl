#!/usr/bin/perl -w
#
# jelsaw.pl -- simple password manager with OAuth 2.0 support
#
# Take a gpg-encrypted INI-file from @VAULTS and copy one of its secrets
# (a password for example) to the primary X selection, from which you
# can paste it in another window with the middle button of the mouse.
#
# To run the Config::IniFiles perl module (libconfig-inifiles-perl in Debian),
# GnuPG and xsel(1) must be installed.  If the clipboard is not used, the
# program doesn't have to run in X.
#
# The interactive mode needs Term::ReadLine, and the OAuth functionality
# deoends on LWP.
#
# Usage: <<<
# jelsaw.pl [--timeout|-t <seconds>] <vault> [<section>/][<key>]
#   Copy the secret value of <key> ("password" by default) from one of the
#   <vault>'s <section>s (the "default" one if unspecified).  <vault> is
#   the case-insensitive prefix of an INI-file's name under @VAULTS which
#   contains the secrets (~/.config/jelsaw hardwired).  You have <seconds>
#   time (5 by default) to paste the secret value.  After that the program
#   exits and the secret is deleted from the selection.  To finish earlier,
#   press <Enter>.  -t 0 disables the timeout.
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
# jelsaw.pl --interactive|-i <vault>
#   Open the <vault> then prompt for your commands to see or copy secrets.
#   Enter '?' to see the options.
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
# Prints an URL for you to follow.  Once your OAuth provider has authenticated
# you and confirmed the access request, it will return an authentication code,
# which you need to enter this program.  Then jelsaw will exchange this code
# for a refresh token and copy it to the clipboard.  From there you can add it
# to the vault.
#
# See this guide how to set up an oauth2_client_id and secret:
# https://gitlab.com/muttmua/mutt/-/blob/master/contrib/mutt_oauth2.py.README
# >>>

# Modules
use strict;
use IPC::Open2;
use Getopt::Long;
use Config::IniFiles;

# Constants
my @VAULTS = ("$ENV{'HOME'}/.config/jelsaw");
my @GPG = qw(gpg --batch --quiet --decrypt --);
my @XSEL = qw(xsel --input --logfile /dev/null --nodetach);
my $OAUTH2_REDIR = "urn:ietf:wg:oauth:2.0:oob";

# Private variables
my $Opt_timeout;

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
      	print $fh "See $0 for the complete documentation.";
	print $fh "";
	print $fh "Usage:";
	print $fh "  $me <vault> [[<section>]/][<key>]";
	print $fh "  $me --find|-f <vault>";
	print $fh "  $me --edit|-w <vault>";
	print $fh "  $me --all|-a <vault>";
	print $fh "  $me --view|-v|--copy|-c [<section>]/ <vault>";
	print $fh "  $me --view|-v|--copy|-c [[<section>]/]<key> <vault>";
	print $fh "  $me --copy|-c <section-and/or-key> ",
			"[--timeout|-t <seconds>] <vault>";
	print $fh "  $me --overview|-l <vault>";
	print $fh "  $me --interactive|-i <vault>";

	exit($error);
}

# Find a file matching $prefix in @VAULTS and its subdirectories.
sub find_vault
{
	my $prefix = shift;
	my ($is_absolute, @dirs);
	my ($pattern, @prefix_matches, @full_matches);

	@dirs = @VAULTS;
	$is_absolute = $prefix =~ s!^/+!!;
	if ($is_absolute && $prefix =~ s!(.*)/+!!)
	{	# Only search in dirname($prefix).
		$_ = "$_/$1" for @dirs;

	}
	$pattern = qr!^.*/\Q$prefix\E(.*)\.gpg$!i;

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
		die "More than one $prefix found";
	} elsif (@full_matches == 1)
	{
		return $full_matches[0];
	} elsif (@prefix_matches > 1)
	{
		die "More than one $prefix found";
	} elsif (@prefix_matches == 1)
	{
		return $prefix_matches[0];
	} else
	{
		die "$prefix not found";
	}
}

# Decrypt the contents of $vault and parse it as an INI.
sub load_vault
{
	my $vault = shift;
	my $ini;

	open(GPG, '-|', @GPG, $vault)
		or die "$GPG[0]: $!";
	$ini = Config::IniFiles->new(-file => \*GPG, -fallback => "default");
	close(GPG) # Wait until @GPG finishes to see if there was an error.
		or exit($? >> 8);
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
	my $ini = shift;
	my @completions;

	for my $section ($ini->Sections())
	{
		push(@completions, $section);
		foreach my $key ($ini->Parameters($section))
		{
			push(@completions, $key)
				if $section eq "default";
			push(@completions, "$section/$key")
		}
	}

	# Escape \-es and spaces to make tab-completion work on words
	# containing escaped spaces and backslashes.
	s/( |\\)/\\$1/g
		for @completions;

	return @completions;
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

# Pass $str to xsel(1) to copy it to the clipboard.
sub copy_to_clipboard
{
	use POSIX;
	my $str = shift;
	my $xsel;
	local *XSEL;

	# X might be running on another virtual console.
	exists $ENV{'DISPLAY'}
		or $ENV{'DISPLAY'} = ":0";
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

	if ($Opt_timeout)
	{	# Kill $xsel in $Opt_timeout seconds.
		$SIG{'ALRM'} = sub { die };
		alarm($Opt_timeout);
	}

	# Kill $xsel and exit when the user hits enter.
	eval
	{
		local $SIG{'__DIE__'};
		readline;
		alarm(0);
	};
	kill(TERM => $xsel);
}

# Copy a key from $ini to the clipboard.
sub copy_cmd
{
	my ($ini, $what) = @_;

	copy_to_clipboard(uncomment(retrieve_key(
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
	require LWP::UserAgent;
	require HTTP::Request::Common;
	require JSON::PP;
}

# Acquire a new refresh_token from an OAuth provider.
sub oauth2_new_refresh_token
{
	my ($ini, $section, $cmd) = @_;
	my ($provider, $client_id, $client_secret, $scope, $auth_code);
	my ($url, $http_reply, $json);

	$provider	= retrieve_key($ini, $section, "oauth2_provider");
	$client_id	= retrieve_key($ini, $section, "oauth2_client_id");
	$client_secret	= retrieve_key($ini, $section, "oauth2_client_secret");
	$scope		= retrieve_key($ini, $section, "oauth2_scope");

	# First send the user to get the $auth_code from the $provider.
	$url = URI->new("$provider/o/oauth2/auth");
	$url->query_form(
		client_id	=> $client_id,
		scope		=> $scope,
		response_type	=> "code",
		redirect_uri	=> $OAUTH2_REDIR);

	print $url->as_string();
	do
	{
		local $\ = "";
		print "Visit the URL above and enter the authorization code: ";
	};
	defined ($auth_code = readline(*STDIN))
		or die "Cancelled.";
	chomp($auth_code);

	# Exchange the $auth_code for a refresh_token.
	$http_reply = LWP::UserAgent->new()->request(
		HTTP::Request::Common::POST("$provider/o/oauth2/token",
		[
			client_id	=> $client_id,
			client_secret	=> $client_secret,
			code		=> $auth_code,
			grant_type	=> "authorization_code",
			redirect_uri	=> $OAUTH2_REDIR,
		]));
	$http_reply->is_success()
		or die "$provider: ", $http_reply->status_line();

	$json = JSON::PP::decode_json($http_reply->content());
	defined $$json{"refresh_token"}
		or die "$provider: 'refresh_token' missing";

	if ($cmd eq "view")
	{	# If the access_token is missing, that's odd, but not a
		# fatal error.
		print "Refresh token: ", $$json{"refresh_token"};
		print "Access token:  ", $$json{"access_token"}
			if defined $$json{"access_token"};
	} else
	{
		copy_to_clipboard($$json{"refresh_token"});
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
		copy_to_clipboard($$json{"access_token"});
	}
}

# Main starts here.
my ($opt_find, $opt_edit, $opt_view_all, $opt_overview, $opt_interactive);
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
	'f|find'	=> \$opt_find,
	'w|edit'	=> \$opt_edit,
	'a|all'		=> \$opt_view_all,
	'v|view=s'	=> \$opt_view,
	'c|copy=s'	=> \$opt_copy,
	'A|oauth2'	=> \$opt_oauth2,
	't|timeout=i'	=> \$Opt_timeout,
	'l|overview'	=> \$opt_overview,
	'i|interactive'	=> \$opt_interactive,
);

# Find the $vault to read the secrets from.
@ARGV > 0
	or usage(0);
$vault = find_vault(shift);

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
	exec(@GPG, $vault)
		or die "$GPG[0]: $!";
} elsif (!defined ($ini = load_vault($vault)))
{
	exit 1;
}

if ($opt_overview)
{	# Just show the sections and the keys, but not the vaules.
	overview_cmd($ini);
} elsif ($opt_interactive)
{	# Let the user pick what to view or copy interactively.
	# Only load readline-support in interactive mode.
	require Term::ReadLine;
	my ($term, @completions, @commands);

	$term = Term::ReadLine->new('jelsaw');
	$term->ornaments(0);

	# Set up tab-completion of sections and keys.
	@completions = extract_completions($ini);

	# The first word can be a command.
	@commands = qw(
		edit reveal view copy
		gen-oauth2-refresh-token gen-and-copy-oauth2-refresh-token
		gen-oauth2-access-token gen-and-copy-oauth2-access-token);
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

	# The main loop.
	for (;;)
	{
		my $what;

		# Exit silently if the user hit ^D.
		if (!defined ($what = $term->readline("? ")))
		{
			print "";
			last;
		}

		# Strip whitespace.
		$what =~ s/^\s+//;
		$what =~ s/\s+$//;

		# Add the stripped input to the history or replace it
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

		my $ret = eval
		{
			local $SIG{'__DIE__'} = undef;
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
				print "";
				print "gen-oauth2-refresh-token          ",
					"[<section>]";
				print "    Acquire and print a new refresh ",
					"token from an OAuth provider.";
				print "gen-oauth2-access-token           ",
					"[<section>]";
				print "    Acquire and print a new access ",
					"token.";
				print "gen-and-copy-oauth2-refresh-token ",
					"[<section>]";
				print "    Acquire a new refresh token and ",
					"copy it to the clipboard.";
				print "gen-and-copy-oauth2-access-token  ",
					"[<section>]";
				print "    Acquire a new access token and ",
					"copy it to the clipboard.";
				# >>>
			} elsif ($what eq "")
			{
				overview_cmd($ini);
			} elsif ($what eq "edit")
			{
				my $new;

				# If the edition was successful, reload $ini.
				if (system($ENV{'VISUAL'}, $vault) == 0
					&& defined ($new = load_vault($vault)))
				{
					$ini = $new;
					@completions =
						extract_completions($ini);
				}
			} elsif ($what eq "reveal")
			{
				local $\ = "";
				$ini->OutputConfig();
			} elsif ($what =~ s/^gen(-and-copy)?-oauth2
						-(refresh|access)-token\s*//x)
			{
				my $cmd = defined $1 ? "copy" : "view";
				my $fun = $2 eq "refresh"
					? \&oauth2_new_refresh_token
					: \&oauth2_refresh_access_token;
				init_oauth2();
				$fun->($ini, $what || "default", $cmd);
			} elsif ($what =~ s/^view\s*//)
			{
				view_cmd($ini, unescape($what));
			} else
			{
				$what =~ s/^copy\s*//;
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
	if ($key eq "refresh" || $key eq "refresh_token")
	{
		oauth2_new_refresh_token($ini, $section, $cmd);
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
