#!/usr/bin/perl -w
#
# mafwturbator.pl -- C identifier generator
#
# Synopsis:
#   mafwturbator.pl [-s | -ln] -t [<source.c>...]
#   mafwturbator.pl [-s | -ln] -f [<source.h>...]
#
# This program finds words in C source code and and generates
# new identifiers by randomly concatenating them.
#
# Options:
# -t, --types			Consider words as in "SomeNonsense".
# -f, --functions		... or as in "incredible_function_names".
# -n, --anagrams <number>	The number of identifiers to generate.
#				Defaults to 10.
# -l, --length <number>		The desired length of the identifiers.
#				Defaults to 25.
# -s, --summary			Don't generate but print the list of the
#				words found along with their frequencies.
#				-nl is ignored.
#
# Use this program to discover development possibilities in your program.
# The -s mode can be useful as a kind of source code metric.
#
# EXAMPLES
# [Offensive material deleted]
#
# BUGS
# The output is not a list of anagrams at all, but random garbage.
#

use strict;
use re 'eval';
use List::Util;
use Getopt::Long;

# Regular expressions to match a word.
my $RE_TYPES = qr/([A-Z][a-z]+)/;
my $RE_FUNCS = qr/((?>[a-z0-9]+))_*/;

# Parse all @ARGV files, increasing the frequency counters in $first,
# $middle and $last of all words found as defined by $re.
sub getwords
{
	my ($re, $first, $middle, $last) = @_;

	while (<>)
	{
		# For volumenous input it's probably not the best idea
		# to create a new array at almost all non-matching points.
		# Filter out all g_* identifiers right away.  Accept leading
		# underscores in any case.
		/(?{ [ ] })\b(?!g_)_*
		 (?:$re
		    (?{ push(@{$^R}, $^N), $^R })){2,}\b
		 (?{ $$first{shift(@{$^R})}++;
		     $$last{pop(@{$^R})}++;
		     $$middle{$_}++ foreach @{$^R}})
		/gox;
	}
}

# If $hash is a frequency table and $rnd is a random number up to the sum
# of the frequencies this function returns a random key of $hash.
sub pick
{
	my ($hash, $rnd) = @_;
	my ($key, $val);

	# Reset the hash iterator first.
	keys(%$hash);
	$rnd < $val ? return $key : ($rnd -= $val)
		while ((($key, $val) = each(%$hash)));

	# It may happen that $hash is actually empty
	# but $rnd is infinitesimally greater than zero.
	return undef;
}

# Main starts here
my ($opt_which, $opt_nanals, $opt_analen);

# Parse the command line.
$opt_which = $RE_TYPES;
$opt_nanals = 10;
$opt_analen = 25;
Getopt::Long::Configure(qw(no_getopt_compat bundling no_ignore_case));
die unless GetOptions(
	'types|t'	=> sub { $opt_which = $RE_TYPES },
	'functions|f'	=> sub { $opt_which = $RE_FUNCS },
	'summary|s'	=> sub { $opt_nanals = 0 },
	'anagrams|n=i'	=> \$opt_nanals,
	'length|l=i'	=> \$opt_analen);

if (!$opt_nanals)
{
	my (%all, $len);

	# Print the frequency table.
	getwords($opt_which, \%all, \%all, \%all);
	$len = List::Util::max(map(length, keys(%all)));

	printf("%*s %4u\n", -($len+1), "$_:", $all{$_})
		foreach keys(%all);
} else
{
	my (%first, %last);
	our %inbetween;

	$\ = "\n";
	$, = "_" if $opt_which == $RE_FUNCS;

	getwords($opt_which, \%first, \%inbetween, \%last);
	%first && %last
		or die 'Not enough bullshit';

	# Normalize the frequencies to [0..1] for pick().
	foreach (\(%first, %inbetween, %last))
	{
		my $sum;
		$sum += $_ foreach values(%$_);
		$_ /= $sum foreach values(%$_);
	}

	for (1..$opt_nanals)
	{
		my (@words, $len, $rnd);
		local %inbetween = %inbetween;

		@words = (pick(\%first, rand()), pick(\%last,  rand()));
		$len = length($words[0]) + length($words[1]);

		# Don't pick() words from %inbeteen we chose as pre-
		# or postfix.
		$rnd  = 1;
		$rnd -= delete $inbetween{$_} || 0 for @words;

		# Stop adding words %inbetween when we run out of words
		# or when the desired $opt_analen is reached.
		while ($len < $opt_analen && $rnd > 0)
		{
			my $word;

			defined ($word = pick(\%inbetween, rand($rnd)))
				or last;
			splice(@words, -1, 0, $word);
			$len += length($word);
			$rnd -= delete $inbetween{$word};
		}

		print @words;
	}
}

# End of mafwturbator.pl
