#!/usr/bin/perl -w
#
# modinject.pl -- embed .pm:s in scripts
#
# This utility is handy when your program is partitioned into modules,
# but you'd like to ship it as a single executable.  It works by hooking
# into the import mechanism of perl, running the BEGIN:ning of the script,
# taking note of the symbols imported into the main package, and finally
# writing a combined script with proper symbol assignments.
#
# Synopsis:
#   modinject.pl <script.pl> <modules.pm>...
#
# If module1.pm depends on module2.pm both of them need to be listed in
# reverse order (ie. module2.pm first).
#
# Presumptions and limitations:
# -- parameters of the use:d packages are symbols to be exported,
#    and those parameters are specified in a certain format:
#    * use Akarmi;
#    * use Valami ("alpha", "beta");
#    * use Semmit qw(
#		alpha
#		beta
#		gamma);
#    ie. the statement lasts until the next line not starting with whitespace
#    (so that we can recognize and skip over it)
# -- the packages don't "use" Exporter, but "require" it (otherwise it will
#    probably override our import() definitions)
# -- script.pl has an empty line before the code starts indicating the point
#    where the <modules.pm> can be pasted
#

# We'll eval <script.pl>, let's not pollute the main package's namespace.
package ModInjector;
use strict;

# This needs to be defined before lexical-scoped variables are declared,
# otherwise the evaluated code will see them.
sub safe_eval { package main; eval shift or die }

# Translates a module file name to a package name
# (eg. Some/Thing.pm => Some::Thing).
sub mod2pkg
{
	my $mod = shift;
	$mod =~ s!/+!::!g;
	$mod =~ s/\.pm$//;
	return $mod;
}

# { $module_file_name => [ [ $user_package => @imported_symbols ], ... ] }
my %Packages;

# Parse the command line.
die "usage: modinject.pl <script> <modules>..."
	if @ARGV < 1;
my $Script = shift;
my @Modules = @ARGV;

# If the $Script doesn't use any @Modules we can dump it as is.
if (!@Modules)
{
	system("cat", $Script);
	chmod(0755, *STDOUT);
	exit;
}

# Create an import() function for each <modules.pm> which captures the
# parameters passed to "use" and registers them in %Packages.
foreach my $mod (@Modules)
{
	no strict qw(refs);

	# Assume that Exporter is require:d, not use:d (or not used at all),
	# so it won't override this import() function.
	my $package = mod2pkg($mod);
	*{"${package}::import"} = sub
	{
		# That's our package.  Determine the $caller's one.
		shift;

		# This $package is defined in this $mod:ule and is $import:ed
		# by $caller.
		my $caller = caller();
		my $import = [ $caller ];
		push(@{$Packages{$mod}}, $import);

		# These @_ symbols (at least they're though to be symbols)
		# are imported from $package's to the $caller's namespace.
		# Strip the funny character.
		foreach (map(/^(?:[\$@%&])?(.*)$/, @_))
		{	# We need to do actually export the symbol,
			# otherwise the compilation of the $Script
			# might fail due to undeclared variables.
			*{"${caller}::$_"} = \*{"${package}::$_"};
			push(@$import, $_);
		}
	};
}

# Create a shell script which cat:s $Script (until __DATA__ if present)
# and adds a BEGIN { } section calling kikop().  The purpose is to call
# the function after all use:s have been processed but without executing
# the script itself.
my $prg;
$prg  = q(sed -ne '/^__DATA__$/q; p');
$prg .= " ";
$prg .= qq("$Script");
$prg .= "; ";
$prg .= "echo '";
$prg .= "BEGIN { ModInjector::kikop() }";
$prg .= "';";
$prg  = qx($prg);

# Run $prg, which should end in kikop(), not executing the script actually.
# If the call to kikop() is prevented somehow (e.g. because of an exit())
# call it ourselves as a final act.
safe_eval($prg);
kikop();

# Construct and print the modinject:ed $Script.
sub kikop
{
	local *SCRIPT;
	open(SCRIPT, '<', $Script)
		or die "$Script: $!";

	# Find the first empty line (which is supposed to delimit
	# the introductional comments).
	while (<SCRIPT>)
	{
		print;
		last if $_ eq "\n";
	}

	# Paste the @Modules, starting with the innermost dependant.
	foreach my $mod (@Modules)
	{	# Not all $mod:ules are in %Packages.
		my $import  = delete $Packages{$mod};
		my $package = mod2pkg($mod);

		# Confine the $mod:ule in its own lexical scope.
		print "do { # Pasted by modinject.pl from $mod\n";
		system("cat", $mod);

		# Iterate through [ $user_package => @imported_symbols ], ...
		# and import the symbols from $package into the $caller's
		# namespace.
		foreach (@$import)
		{
			my $caller = shift(@$_);
			next if !@$_;

			print "\n";
			print "# Export symbols to $caller.\n";
			print "BEGIN\n";
			print "{\n";

			# "*CallerPackage::akarmi = *UsedPackage::akarmi;"
			print "\t", '*', $caller, '::', $_, ' = ',
					'*', $package, '::', $_, ";\n"
				foreach @$_;
			print "}\n";
		}
		print "}; # End of $mod\n";
	}
	print "\n";

	# Make $uses match "use" and "require" statements.
	my $uses;
	$uses = join('|', map("\Q$_\E", map(mod2pkg($_), @Modules)));
	$uses = "^(?:use|require)\\s+($uses)";
	$uses = qr/$uses/o;

	# Write the remains of the main SCRIPT.
	while (<SCRIPT>)
	{	# Pass through non-use-like statements.
		if ($_ !~ $uses)
		{
			print;
			next;
		}

		# Found a "use" or a "require" of a pasted module.  Skip it.
		# The statement is supposed to end until the next line not
		# beginning of a white space, such as:
		for (;;)
		{
			$_ = <SCRIPT>;
			next if /^\s/;

			# Found the next line not starting with whitespace.
			# Continue dumping the SCRIPT.
			print;
			last;
		}
	}

	# Make the output SCRIPT executable.
	chmod(0755, *STDOUT);
	exit;
}

# We're done!
# End of modinject.pl
