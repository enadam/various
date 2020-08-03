#!/usr/bin/perl -w
#
# pwman.pl -- simple password manager
#
# Take a gpg-encrypted INI-file from @VAULTS and copy one of its secrets
# (a password for example) to the primary X selection, from which you
# can paste it in another window with the middle button of the mouse.
#
# To run the Config::IniFiles perl module (libconfig-inifiles-perl in Debian),
# GnuPG and xsel(1) must be installed.
#
# Usage: <<<
# pwman.pl [--timeout|-t <seconds>] <vault> [[<section>/]<key>]
#   Copy the secret value of <key> ("password" by default) from one of the
#   <vault>'s <section>s (the "default" one if unspecified).  <vault> is
#   the case-insensitive prefix of an INI-file's name under @VAULTS which
#   contains the secrets (~/.config/pwman hardwired).  You have <seconds>
#   time (5 by default) to paste the secret value.  After that the program
#   exits and the secret is deleted from the selection.  -t 0 disables the
#   timeout.  This case press <Enter> to quit when you're finished.
#
# pwman.pl --find|-f <vault>
#   Return the vault that woule be opened.
#
# pwman.pl --edit|-w <vault>
#   Edit the <vault> with your $VISUAL editor.
#
# pwman.pl --all|-a <vault>
#   Show the <vault> as-is in plaintext.
#
# pwman.pl --view|-v <section> <vault>
#   Show <section>'s all keys and secret values.
#
# pwman.pl --view|-v [<section>/]<key> <vault>
#   Show the specified <key>'s secret value.
#
# pwman.pl --copy|-c [<section>/]<key> [--timeout|-t <seconds>] <vault>
#   Copy the specified <key>'s secret value.
#
# pwman.pl --overview|-l <vault>
#   Show the sections and keys in the <vault>, but not the secret values.
#
# pwman.pl --interactive|-i <vault>
#   Open the <vault> then prompt for your commands to see or copy secrets.
#   Enter '?' to see the options.
# >>>
#
# A <vault> file (for example "shops.gpg") could be: <<<
#
# # This is the default <section>.
# username = me@mine.org
# password = hihihi
#
# [site1]
# security question = Who are you?
# answer = Dunno
#
# [site2]
# security question = What do you want?
# answer = Everything
#
# Examples:
#
# $ pwman.pl shops
#   Copies "hihihi" from the default section.
#
# $ pwman.pl shops username
#   Copies "me@mine.org".
#
# $ pwman.pl shops site1/answer
#   Copies "Dunno".
#
# $ pwman.pl shops -v username
#   Prints "me@mine.org".
#
# $ pwman.pl shops -v site1
#   Prints:
# security question = Who are you?
# answer            = Dunno
#
# $ pwman.pl shops -l
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

# Modules
use strict;
use IPC::Open2;
use Getopt::Long;
use Config::IniFiles;

# Constants
my @VAULTS = ("$ENV{'HOME'}/.config/pwman");
my @GPG = qw(gpg --batch --quiet --decrypt --);
my @XSEL = qw(xsel --input --logfile /dev/null --nodetach);

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
	print $fh "  $me <vault> [[<section>/]<key>]";
	print $fh "  $me --find|-f <vault>";
	print $fh "  $me --edit|-w <vault>";
	print $fh "  $me --all|-a <vault>";
	print $fh "  $me --view|-v <section> <vault>";
	print $fh "  $me --view|-v [<section>/]<key> <vault>";
	print $fh "  $me --copy|-c [<section>/]<key> ",
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

# Print $ini's sections and keys.
sub overview
{
	my $ini = shift;

	for my $section ($ini->Sections())
	{
		print $section, ":";
		print "  ", $_
			foreach $ini->Parameters($section);
	}
}

sub view_or_copy
{
	my ($ini, $what, $which) = @_;
	my ($section, $key, $val);

	# $which -> $section, $key
	if (ref $which)
	{	# Already decomposed.
		($section, $key) = @$which;
	} elsif ($which =~ m!^(.*)/(.*)$!)
	{	# $which is "$section/$key"
		($section, $key) = ($1, $2);
	} elsif ($what eq "view" && $ini->SectionExists($which))
	{	# Show the whole $section.
		my @keys = $ini->Parameters($section = $which);
		my $maxwidth;

		# Align the key values.
		defined $maxwidth && $maxwidth > length($_)
				or $maxwidth = length($_)
			foreach @keys;
		printf("%-*s = %s\n", $maxwidth, $_, $ini->val($section, $_))
			foreach @keys;
		return;
	} else
	{	# Take $key from the default section.
		($section, $key) = ("default", $which);
	}

	if (!defined ($val = $ini->val($section, $key)))
	{
		die "$section/$key: no such key";
	} elsif ($what eq "view")
	{	# Just show $val.
		print $val;
	} else
	{	# Pass $val to xsel(1) to copy it to the clipboard.
		use POSIX;
		my $xsel;
		local *XSEL;

		# Delete trailing comment.
		$val =~ s/\s+#\s+.*$//;
		$val =~ s/\\#/#/g;

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
		print XSEL $val;
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
}

# Main starts here.
my ($opt_find, $opt_edit, $opt_view_all, $opt_view, $opt_copy);
my ($opt_overview, $opt_interactive);
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
	't|timeout=i'	=> \$Opt_timeout,
	'l|overview'	=> \$opt_overview,
	'i|interactive'	=> \$opt_interactive);

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
{	# Copy default/password by default.
	$opt_copy = [ "default", "password" ];
} elsif (@modes > 1)
{	# More than one mode is selected.
	usage();
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
}

# Decrypt and read the contents of $vault into $ini.
open(GPG, '-|', @GPG, $vault)
	or die "$GPG[0]: $!";
$ini = Config::IniFiles->new(-file => \*GPG, -fallback => "default");
close(GPG) # Wait until @GPG finishes to see if there was an error.
	or exit($? >> 8);
if (!defined $ini)
{	# Syntax error in the $vault.
	$, = "\n";
	print STDERR "$vault";
	print STDERR @Config::IniFiles::errors;
	exit 1;
}

if ($opt_overview)
{	# Just show the sections and the keys, but not the vaules.
	overview($ini);
} elsif ($opt_interactive)
{	# Let the user pick what to view or copy interactively.
	for (;;)
	{
		my $what;

		do
		{
			local $\ = "";
			print "? ";
		};

		# Exit silently if the user hit ^D.
		if (!defined ($what = readline))
		{
			print "";
			last;
		}
		chomp($what);

		my $ret = eval
		{
			local $SIG{'__DIE__'} = undef;
			if ($what eq "exit" || $what eq "q")
			{
				return 0;
			} elsif ($what eq "help" || $what eq '?')
			{
				print "help, ?                  - ",
					"???";
				print "exit, q                  - ",
					"I'm done";
				print "edit                     - ",
					"edit the vault file";
				print "<Enter>                  - ",
					"see an overview of the vault";
				print "reveal                   - ",
					"show the entire vault";
				print "view <section>           - ",
					"view the selected <section>";
				print "view [<section>/]<key>   - ",
					"view the selected <key>";
				print "[copy] [<section>/]<key> - ",
					"copy the selected <key> ",
					"to clipboard";
			} elsif ($what eq "")
			{
				overview($ini);
			} elsif ($what eq "edit")
			{
				system($ENV{'VISUAL'}, $vault);
			} elsif ($what eq "reveal")
			{
				local $\ = "";
				$ini->OutputConfig();
			} elsif ($what =~ s/^view\s+//)
			{
				view_or_copy($ini, "view", $what);
			} else
			{
				$what =~ s/^copy\s+//;
				view_or_copy($ini, "copy", $what);
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
} elsif (defined $opt_view)
{
	view_or_copy($ini, "view", $opt_view);
} else
{
	view_or_copy($ini, "copy", $opt_copy);
}

# vim: set foldmethod=marker foldmarker=<<<,>>>:
# End of pwman.pl
