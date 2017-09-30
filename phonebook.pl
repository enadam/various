#!/usr/bin/perl -w
#
# phonebook.pl -- look up someone in LDAP
#
# Search criteria can be name, e-mail address, userid, employee number or
# any combinations thereof.  To make the script work in your environment
# you'll need to customize the $LDAP and $BASEDIR constants.
#
# Synopses:
# 1 Brief invocation:
#   phonebook.pl {-h|--help}
# 2 Look up yourself ($USER):
#   phonebook.pl
# 3 View the records of everyone mentioned:
#   phonebook.pl [-q|-qq] [-d|--delimiter <delimiter>] 
#                [--me|<employee-id>|<nokia-username>|<LDAP-filter>]...
#                [-A <attributes>...]
# 4 Select some records then apply filters:
#   phonebook.pl [-q|-qq] [-d|--delimiter <delimiter>]
#                [--me|<select-uid>|<select-username>]...
#                {{-U|--uid <employee-number>}
#                 |{-u|--user <nokia-username>}
#                 |{-n|--name <full-name>}
#                 |{-m|--mail <e-mail>[@nokia.com]}
#                 |<LDAP-filter>}...
#                [<attributes>]...
#
# Attributes to view can be dn (distinguished name, the unique LDAP address
# of the record), cn (commonName, the person's natural name), mail (company
# e-mail address), employeeNumber (as printed on the badge), nsnSiteCode.
# or in fact, anything else.  If none specified, all attributes are shown.
#
# In the third invocation, simply all records are returned which satisfy
# any of the listed criteria.  Eg. `phonebook.pl --me albert' will print
# both yours and albert's record.
#
# In the fourth invocation --me, <select-uid> and <select-username> works
# the same way.  However, that case you can specify additional filtering
# criteria, which are applied to the selected records.  Moreover, multiple
# criteria of the same kind are conjugated.  Eg. -n "Romeo" -n "Juliette"
# will list both person's records.  You can also employ wildcards: -u 'pi*'
# will return both pista's and pinocchio's records.
#
# By default attribute names are printed along with their values.  Use -q
# to suppress them.  With -qq all records are printed on a single line
# delimited by <delimiter> (", " by default).  The usage of -d implies -qq.
#
# You may omit the "@nokia.com" part of <e-mail> addresses.  <full-name>s
# can be spelled with flying accents (a' => á and so on).
#
# <LDAP-filter>s are for attributes not recognized by command line switches.
# For example to get all the Adams working in the Budapest office, try:
# `phonebook.pl -q '(&(nsnSiteCode=HUBUDAA)(mail=adam.*))' cn'.
#
# A silly example showcasing all the features:
#   phonebook.pl					\
#	--me 123456 kocsis				\
#	-n "* Sa'ndor" -n 'Pa:ivi *' -m '*milan*'	\
#	'(nsnSiteCode=FIESPAQ)'				\
#	cn
# It'll take your record, the person's whose employeeNumber is 123456
# and that of whose NSN username is kocsis, filter those whose give name
# is either Sándor or Päivi AND whose e-mail address is '*milan*@nokia.com',
# AND who work in Espoo, and print their natural name(s).
#
# Another silly example:
#   phonebook.pl -q '6145574*' -A cn
# will print the names of all employees whose number starts with 6145574.
#
# This program requires either ldapsearch(1) (ldap-utils package in Debian)
# or the Net::LDAPapi Perl module (libnet-ldapapi-perl).  The latter is
# preferred for performance, but beware that in Wheezy due to a packaging
# error you'll also have to install Convert::ASN1 (libconvert-asn1-perl)
# manually.
#

# Modules
use utf8;
use strict;

# Constants
my $LDAP	= "ldap.internal.network";
my $BASEDIR	= "ou=people,o=company";
my %ACCENTS	=
(
	"a'"	=> 'á', "A'"	=> 'Á',
	"a:"	=> 'ä', "A:"	=> 'Ä',
	"e'"	=> 'é', "E'"	=> 'É',
	"i'"	=> 'í', "I'"	=> 'Í',
	"o'"	=> 'ó', "O'"	=> 'Ó',
	"o:"	=> 'ö', "O:"	=> 'Ö',
	"o\""	=> 'ő', "O\""	=> 'Ő',
	"o~"	=> 'õ', "O~"	=> 'Õ',
	"u'"	=> 'ú', "U'"	=> 'Ú',
	"u:"	=> 'ü', "U:"	=> 'Ü',
	"u\""	=> 'ű', "U\""	=> 'Ű',
);
my $ACCENTRE = join('|', map("\Q$_\E", keys(%ACCENTS)));

# Program code
# Return "(|($field=$_[0])($field=$_[1])...)".
sub mkorfilter
{
	my $field = shift;

	@_ = grep(defined, @_);
	if (!@_)
	{
		return undef;
	} elsif (@_ == 1)
	{
		return "($field=$_[0])";
	} else
	{
		return "(|" . join('', map("($field=$_)", @_)) . ")";
	}
}

# Return "($op $_[0] $_[1] ...)".  Arguments are supposed to be valid
# LDAP filter expressions.
sub mkfilter
{
	my $op = shift;

	@_ = grep(defined, @_);
	if (!@_)
	{
		return undef;
	} elsif (@_ == 1)
	{
		return $_[0];
	} else
	{
		return "($op" . join('', @_) . ")";
	}
}

# Main starts here
my $dir;
my $terse = 0;
my $delimiter = ", ";
my ($first_form, $found);
my ($has_net_ldap, $has_convert_asn1);
my (%filter, $filter, @filter, @select, @custom);

# Parse the command line and get @select, @filter and @custom.
# If no arguments are given, print the record of uid=$USER.
$first_form = 1;
@ARGV || push(@ARGV, "--me");
while (@ARGV)
{
	my ($opt, $arg);

	# Process options without argument.
	$opt = shift;
	if ($opt eq "-h" || $opt eq "--help")
	{	# Print help.
		print	"phonebook.pl [-q|-qq] [-d <delimiter>]\n",
			"             [--me|<employee-id>|<nokia-username>",
					"|<LDAP-filter>]...\n",
			"             [-A <attributes>...]\n";
		print	"phonebook.pl [-q|-qq] [-d <delimiter>]\n",
			"             [--me|<select-uid>|<select-username>]",
					"...\n",
			"             {{-U|--uid <employee-number>}",
					"|{-u|--user <nokia-username>}\n",
			"              |{-n|--name <full-name>}",
					"|{-m|--mail <e-mail>[\@nokia.com]}\n",
			"              |<LDAP-filter>}...\n",
			"             [<attributes>]...\n";
		print "<attributes>: ",
			join(", ", qw(dn cn mail employeeNumber nsnSiteCode)),
			"\n";
		exit 0;
	} elsif ($opt eq "-q")
	{	# Be terse.
		$terse++;
		next;
	} elsif ($opt eq "-qq")
	{	# One-line output.
		$terse = 2;
		next;
	} elsif ($opt eq "--me")
	{	# uid=$USER
		push(@{$filter{'uid_or_enum'}}, $ENV{'USER'});
		next;
	} elsif ($opt =~ /^[0-9*]+$/)
	{	# employeeNumber=$1
		push(@{$filter{'enum_or_uid'}}, $opt);
		next;
	} elsif (substr($opt, 0, 1) eq "(")
	{	# Custom search criteria.
		push(@custom, $opt);
		next;
	} elsif ($opt eq "-A")
	{	# The rest of @ARGV are <attributes>...
		last;
	} elsif (substr($opt, 0, 1) ne "-")
	{
		if ($first_form)
		{	# Plain <uid>
			push(@{$filter{'uid_or_enum'}}, $opt);
			next;
		} else
		{	# Beginning of <attributes>...
			unshift(@ARGV, $opt);
			last;
		}
	}

	# Process options with an argument.
	if (!@ARGV)
	{
		print STDERR "$0: $opt: required argument missing\n";
		exit 1;
	}

	$arg = shift;
	if ($opt eq "-d" || $opt eq "--delimiter")
	{	# Don't touch $first_form.
		$delimiter = $arg;
		$terse = 2;
		next;
	} elsif ($opt eq "-U" || $opt eq "--uid")
	{	# Print the record of employeeNumber=$1.
		push(@{$filter{'employeeNumber'}}, $opt);
	} elsif ($opt eq "-u" || $opt eq "--user")
	{	# Print the record of uid=$1.
		push(@{$filter{'uid'}}, $arg);
	} elsif ($opt eq "-n" || $opt eq "--name")
	{	# Print the record of cn=$1.
		# Resolve ASCII replacements of accented characters.
		$arg =~ s!($ACCENTRE)!$ACCENTS{$1} || $1!ego;
		push(@{$filter{'cn'}}, $arg);
	} elsif ($opt eq "-m" || $opt eq "--mail")
	{	# Print the record of mail=$1 or nsnPrimaryEmailAddress=$1.
		index($arg, "@") >= 0
			or $arg .= "\@nokia.com";
		push(@{$filter{'mail'}}, $arg);
	} else
	{	# $opt starts with a '-' but we don't know it.
		print STDERR "$0: $opt: unknown option\n";
		exit 1;
	}
	$first_form = 0;
}

# %filter -> @select and @filter.  The difference is that @select:s members
# are OR:ed while @filter:s are AND:ed.  Only 'enum_or_uid' or 'uid_oe_enum'
# can be @select:ors.
if (defined ($filter = delete $filter{'enum_or_uid'}))
{       
	if (!defined $filter{'uid_or_enum'}
		&& @$filter == 1 && $$filter[0] =~ /^\d+$/)
	{	# We have a single employeeId selection criteria,
		# so we can query the record directly.       
		$dir = "employeeNumber=$$filter[0],ou=internal,$BASEDIR";
	} else
	{       
		push(@select, map("(employeeNumber=$_)", @$filter));
	}
}

# uid_or_enum is the other selection criterium.
defined ($filter = delete $filter{'uid_or_enum'})
	and push(@select, map("(uid=$_)", @$filter));

# The rest are @filter criteria.
if (defined ($filter = delete $filter{'employeeNumber'}))
{
	if (!defined $dir && @$filter == 1 && $$filter[0] =~ /^\d+$/)
	{	# Single employeeId criteria, we can query it directly.
		$dir = "employeeNumber=$$filter[0],ou=internal,$BASEDIR";
	} else
	{
		push(@filter, mkorfilter("employeeNumber", @$filter));
	}
}

# mail also has another attribute (nsnPrimaryEmailAddress)
defined ($filter = delete $filter{'uid'})
	and push(@filter, mkorfilter("uid", @$filter));
defined ($filter = delete $filter{'cn'})
	and push(@filter, mkorfilter("cn", @$filter));
defined ($filter = delete $filter{'mail'})
	and push(@filter, mkfilter('|',
		map("(mail=$_)", @$filter),
		map("(nsnPrimaryEmailAddress=$_)", @$filter)));

# Put together the final $filter.
$filter = mkfilter('&', mkfilter('|', @select), @filter, @custom);
@filter = defined $filter ? ($filter) : ();
if (!defined $dir)
{
	$dir = $BASEDIR;
	if (!@filter)
	{	# We don't want to dump the entire directory.
		print STDERR "$0: no search criteria\n";
		exit 1;
	}
}

# Can we use Net::LDAPapi?
if (eval { require Net::LDAPapi; })
{
	Net::LDAPapi->import();
	$has_net_ldap = $has_convert_asn1 = 1;
} elsif ($@ =~ m!^Can't locate Convert/ASN1.pm in \@INC\b!)
{	# libnet-ldapapi-perl in Debian has a nasty missing dependency
	# declaration on libconvert-asn1-perl.
	$has_net_ldap = 1;
}

# Run
if ($has_net_ldap && $has_convert_asn1)
{	# Access the LDAP server directly with Net::LDAPapi.
	my ($ldap, $msgid);

	# Connect and log in anonymously.
	$ldap = Net::LDAPapi->new($LDAP);
	$ldap->bind_s() == LDAP_SUCCESS()
		or die "Couldn't log into $LDAP";

	# Issue the search command.
	defined ($msgid = do
	{	# Some warn:s we have no influence on need to be suppressed.
		local $SIG{'__WARN__'} = sub
		{
			$_[0] =~ m!^Use of uninitialized value in (?:numeric eq \(==\)|subroutine entry) at /usr/lib/perl5/Net/LDAPapi.pm line \d+\.$!
				or warn @_;
		};
		$ldap->search($dir, LDAP_SCOPE_SUBTREE(), $filter, \@ARGV, 0);
	}) or die $ldap->errstring();

	# Process the results.
	for (;;)
	{
		if (!defined $ldap->result($msgid, 0, -1))
		{
			die $ldap->errstring();
		} elsif ($$ldap{'status'} == LDAP_RES_SEARCH_RESULT())
		{
			last;
		} elsif ($$ldap{'status'} != LDAP_RES_SEARCH_ENTRY())
		{
			next;
		}

		for (my $ent = $ldap->first_entry(); $ent;
			$ent = $ldap->next_entry())
		{
			my @output;

			$found = 1;
			if (!$terse)
			{
				print "dn: ", $ldap->get_dn(), "\n";
			} elsif (!@ARGV || grep($_ eq "dn", @ARGV))
			{
				push(@output, $ldap->get_dn());
			}

			# Process an entry's attributes.
			for (my $attr = $ldap->first_attribute();
				defined $attr;
				$attr = $ldap->next_attribute())
			{
				next if $attr eq "objectClass";

				my $v = join(", ", $ldap->get_values($attr));
				if ($terse)
				{
					push(@output, $v);
				} else
				{
					print "    $attr: $v\n";
				}
			}

			if ($terse > 1)
			{
				print join($delimiter, @output), "\n";
			} elsif ($terse)
			{
				print join("\n", @output), "\n";
			}
		}
	}
} elsif (!open(LDAP, "-|", "ldapsearch",
	"-h", $LDAP, "-x", "-b", $dir, @filter, @ARGV))
{	# Can't access the ldap server by any means.
	print STDERR "install ", $has_net_ldap
		? "Convert::ASN1 (libconvert-asn1-perl)"
		: "ldapsearch (ldap-utils)",
		"\n";
	exit 1;
} else
{	# Use ldapsearch(1).
	my @output;

	require MIME::Base64;
	while (<LDAP>)
	{
		# Skip empty lines, comments and objectClass:es.
		next if $_ eq "\n";
		next if substr($_, 0, 1) eq "#";
		next if /^(?:objectClass|search|result): /;
		$found = 1;

		# Skip the dn heading in $terse mode.
		if (/^dn: /)
		{
			print join($delimiter, @output), "\n"
				if @output;
			@output = ();
			next if $terse && @ARGV && !grep($_ eq "dn", @ARGV);
		}

		# Decode base64-encoded values (cn in particular).
		s/^(\w+:):(\s*)(.*)$/
			$1 . $2 . MIME::Base64::decode_base64($3)/e;

		# Format output and print.
		$terse	? s/^\w+:\s*//
			: s/^(?!dn: )/    /;
		if ($terse > 1)
		{
			chomp;
			push(@output, $_);
		} else
		{
			print;
		}
	}

	print join($delimiter, @output), "\n"
		if @output;
}

# Done
print STDERR "$0: no results\n"
	if !$found && !$terse;
exit !$found;

# End of phonebook.pl
