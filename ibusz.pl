#!/usr/bin/perl -w
#
# ibusz.pl -- help D-BUS message reconstruction
#
# This program helps you to resend D-BUS messages captured by dbus-monitor.
# It takes the output of dbus-monitor and translates it into Python code
# fragments which reconstruct the same messages.
#
# Synopsis:
#	./ibusz.pl [-session|-system] [<input>]
#
# Print the Python fragments on the standard output.  If either -session
# or -system is supplied then a full program is written, which not only
# recreates the messages but also sends them on the named bus.  If <input>
# is omitted, standard input is read.
#
# The reconstructed message types are signals and method calls.  Error and
# method reply messages are recognized and parsed but ignored because they
# are not useful for this purpose.  Parsing errors are not handled well.
#

use strict;
use Parse::RecDescent;

# Parse the command line.
my $bus;
if (@ARGV && $ARGV[0] eq '-session')
{
	$bus = 'SessionBus';
	shift;
} elsif (@ARGV && $ARGV[0] eq '-system')
{
	$bus = 'SystemBus';
	shift;
}

# Write a full program?
print <<'EOT' if $bus;
#!/usr/bin/python

import dbus

msgs = []
EOT

chmod(0755, *STDOUT) if $bus;

# Parse the input and print the D-BUS message construction statements.
undef $/;
$\ = "\n";
$" = "";
Parse::RecDescent->new(<<'EOT'
	top: message(s) { print "#jo vagy nalam szivi" }
	message: msgtype values
	{
		return if !$item{msgtype};
		print "msgs.append($item{msgtype})";
		print "msgs[-1].append($item{values})"
			if $item{values};
	}

	msgtype: signal | method | return | error
	signal: "signal"
		sender "->" destination serial
		path ";" interface ";" member
	{
		"dbus.lowlevel.SignalMessage("
			. join(', ', map("\"$_\"",
				@item{qw(path interface member)}))
			. ")";
	}
	method: "method" "call"
		sender "->" destination serial
		path ";" interface ";" member
	{
		"dbus.lowlevel.MethodCallMessage("
			. join(', ', map("\"$_\"",
				@item{qw(destination path interface member)}))
			. ")";
	}
	return: "method" "return" sender "->" destination reply
		{ "" }
	error: "error" sender "->" destination errcode reply
		{ "" }

	values: value(s?)
		{ join(', ', @{$item[1]}) }
	value: variants (basic | container)[$item[1]]
	variants: ("variant")(s?)
		{ scalar(@{$item[1]}) }

	OUT:
	{
		$arg[-1] ? "dbus.types.$arg[0]($arg[1], "
				. "variant_level=$arg[-1])"
			 : "dbus.types.$arg[0]($arg[1])"
	}

	basic: "boolean" boolean
		OUT['Boolean', ucfirst($item{boolean}), @arg]
	basic: "byte" unsigned
		OUT['Byte', $item{unsigned}, @arg]
	basic: "int32" integer
		OUT['Int32', $item{integer}, @arg]
	basic: "uint32" unsigned
		OUT['UInt32', $item{unsigned}, @arg]
	basic: "int64" integer
		OUT['Int64', $item{integer}, @arg]
	basic: "uint64" unsigned
		OUT['UInt64', $item{unsigned}, @arg]
	basic: "string" string 
		OUT['String', $item{string}, @arg]
	basic: "object" "path" string
		OUT['ObjectPath', $item{string}, @arg]

	container: empty_dict_with_sig | empty_array_with_sig | empty_array |
			dictionary | array | struct
	empty_dict_with_sig: 'array' '[' '(a{' basictype type '})' ']'
		OUT['Dictionary', '[], signature="'
			. $item{basictype}.$item{type}
			. '"', @arg]
	empty_array_with_sig: 'array' '[' '(a' type ')' ']'
		OUT['Array', '[], signature="'.$item{type}.'"', @arg]
	empty_array: 'array' '[' ']'
		OUT['Array', '[], signature="s"', @arg]
	array: 'array' '[' values ']'
		OUT[ucfirst($item[0]), "[$item{values}]", @arg]
	struct: 'struct' '{' values '}'
		OUT[ucfirst($item[0]), "[$item{values}]", @arg]
	dictionary: 'array' '[' dict_entry(s) ']'
		OUT[ucfirst($item[0]), "{".join(', ', @{$item[3]})."}", @arg]
	dict_entry: 'dict' 'entry' '(' basic[0] value ')'
		{ "$item{basic}: $item{value}" }

	sender:		"sender=" <skip:''> address
	sender:		"sender=(null sender)"
			{ "" }
	destination:	"dest=" <skip:''> address
	destination:	"dest=(null destination)"
			{ "" }
	serial:		"serial="	<skip:''> unsigned
	reply:		"reply_serial="	<skip:''> unsigned
	path:		"path="		<skip:''> slashed
	interface:	"interface="	<skip:''> dotted
	member:		"member="	<skip:''> /\w+/
	errcode:	"error_name="	<skip:''> dotted

	address:	/:?\w+(?:\.\w+)+/
	dotted:		/\w+(?:\.\w+)+/
	slashed:	m!(?:/\w+)+!

	boolean:	"true" | "false"
	integer:	/[+-]?\d+/
	unsigned:	/\d+/
	string:		/"(?:\\.|[^"])*"/

	signature:	type(s)			{ "@{$item[1]}"		}
	type:		arraytype | structtype | dicttype | basictype
	arraytype:	'a' type		{ "@item[1..$#item]"	}
	structtype:	'(' signature ')'	{ "@item[1..$#item]"	}
	dicttype:	'a' '{' signature '}'	{ "@item[1..$#item]"	}
	basictype:	/\w/
EOT
)->top(<>);

# Finish the program.
print <<"EOT" if $bus;

for msg in msgs:
	dbus.$bus().send_message(msg)
EOT

# End of ibusz.pl
