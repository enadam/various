#!/usr/bin/perl -w
#
# depcap.pl -- annotated tshark -V {{{
#
# This program reads the XML (PDML) output of Wireshark and displays its
# dissection along with the captured bytes mapped with the protocol fields.
# In practical terms, it will tell you where each field is located in the
# packet and which bytes constitute it.  A sample output will make it clear:
#
# {{{
# ------------- General information
# [...]
# ------------- Frame 1: 432 bytes on wire (3456 bits), 432 bytes captured (3456 bits)
# [...]
# ------- [0+0] Raw packet data
# 0000 --------     No link information available
# ------ [0+20] Internet Protocol Version 4, Src: 127.0.0.1 (127.0.0.1), Dst: 127.0.0.1 (127.0.0.1)
# 0000 ......45     Version: 4
# -""- ---""---     Header length: 20 bytes
# ------- [1+1]     Differentiated Services Field: 0x00 (DSCP 0x00: Default; ECN: 0x00: Not-ECT (Not ECN-Capable Transport))
# 0001 ......00         0000 00.. = Differentiated Services Codepoint: Default (0x00)
# -""- ---""---         .... ..00 = Explicit Congestion Notification: Not-ECT (Not ECN-Capable Transport) (0x00)
# 0002 ....01b0     Total Length: 432
# 0004 ....0000     Identification: 0x0000 (0)
# ------- [6+1]     Flags: 0x00
# 0006 ......00         0... .... = Reserved bit: Not set
# -""- ---""---         .0.. .... = Don't fragment: Not set
# -""- ---""---         ..0. .... = More fragments: Not set
# 0007 ......00     Fragment offset: 0
# 0008 ......10     Time to live: 16
# 0009 ......84     Protocol: SCTP (132)
# ------ [10+2]     Header checksum: 0xaac8 [correct]
# 000a ....aac8         Good: True
# -""- ---""---         Bad: False
# 000c 7f000001     Source: 127.0.0.1 (127.0.0.1)
# -""- ---""---     Source or Destination Address: 127.0.0.1 (127.0.0.1)
# -""- ---""---     Source Host: 127.0.0.1
# -""- ---""---     Source or Destination Host: 127.0.0.1
# 0010 7f000001     Destination: 127.0.0.1 (127.0.0.1)
# -""- ---""---     Source or Destination Address: 127.0.0.1 (127.0.0.1)
# -""- ---""---     Destination Host: 127.0.0.1
# -""- ---""---     Source or Destination Host: 127.0.0.1
# -------------     Source GeoIP: Unknown
# -""- ---""---     Destination GeoIP: Unknown
# ----- [20+28] Stream Control Transmission Protocol, Src Port: diameter (3868), Dst Port: 4321 (4321)
# 0014 ....0f1c     Source port: 3868
# 0016 ....10e1     Destination port: 4321
# 0018 00000000     Verification tag: 0x00000000
# -------------     Port: 3868
# -------------     Port: 4321
# 001c 00000000     Checksum: 0x00000000 (not verified)
# ----- [32+16]     DATA chunk(ordered, complete segment, TSN: 0, SID: 0, SSN: 0, PPID: 0, payload length: 384 bytes)
# ------ [32+1]         Chunk type: DATA (0)
# 0020 ......00             0... .... = Bit: Stop processing of the packet
# -""- ---""---             .0.. .... = Bit: Do not report
# ------ [33+1]         Chunk flags: 0x03
# 0021 ......01             .... ...1 = E-Bit: Last segment
# -""- ---""---             .... ..1. = B-Bit: First segment
# -""- ---""---             .... .0.. = U-Bit: Ordered delivery
# -""- ---""---             .... 0... = I-Bit: Possibly delay SACK
# 0022 ....0190         Chunk length: 400
# 0024 00000000         TSN: 0
# 0028 ....0000         Stream Identifier: 0x0000
# 002a ....0000         Stream sequence number: 0
# 002c 00000000         Payload protocol identifier: not specified (0)
# [...]
#
# Legend:
#   ------------- General information
#   # No packet data is associated with this information.
#   ------ [0+20] Internet Protocol Version 4, Src: 127.0.0.1 (127.0.0.1),
#   # This header or field starts at the zeroth byte and is 20 bytes long.
#   0000 ......45     Version: 4
#   # The zeroth byte is 0x45, which telss this is an IPv4 header.
#   -""- ---""---     Header length: 20 bytes
#   # Two pieces of information are encoded in the same byte.
# }}}
#
# Synopses:
#   depcap.pl [<PDML>]
#   # Read the PDML from the specified file or from the standard input.
#   depcap.pl [<tshark-options>] -r <PCAP>
#   # Dissect <PCAP> with tshark(1) and eat its output.
#
# wireshark(1) (the GUI) has a similar feature.  However, depcap's mapping is
# much more explicit and it also display sizes and offsets of protocol and
# subfield headers.  Not to mention that being basically an XML parser, this
# program's dependencies are much-much lighter and it doesn't require a GUI.
# }}}

# Modules
use strict;
use HTML::Parser;

# Private variables
my (@Tags, $Proto);
my ($Preindent, $Fields);
my ($Prevpos, $Prevdata);

# Private functions
# Return the indentation whitespace for the description.
sub indent
{
	return ' ' x (4*($Preindent + @Tags - 2));
}

# Print a protocol or a subfield header:
#      [48+384] Diameter Protocol
sub print_header
{
	my $tag = shift;
	my $str = "[$$tag[0]+$$tag[1]]";
	printf("%s %s %s%s\n",
		'-' x ((8+4) - length($str)), $str,
		indent(), $$tag[2]);
	$Prevpos = $Prevdata = undef;
}

# Print a regular field.
# 0031 ..000180     Length: 384
# 0044 00000107     AVP: Session-Id(263) l=53 f=-M- val=...
# 0048 40000035
# 004c 69706475
# 0050 302e6674
# ...
sub print_field
{
	my $tag = shift;

	if (!defined $$tag[0])
	{	# No position.
		printf("------------- %s%s\n", indent(), $$tag[2]);
		$Prevpos = $Prevdata = undef;
		return;
	} elsif (defined $Prevpos && $Prevpos > $$tag[0])
	{	# Back in time.
		printf("------------- %s%s\n", indent(), $$tag[2]);
		return;
	} elsif (!defined $$tag[1] || !length($$tag[1]))
	{	# No data.
		printf("%.4x -------- %s%s\n", $$tag[0],
			indent(), $$tag[2]);
		($Prevpos, $Prevdata) = ($$tag[0], undef);
		return;
	} elsif (defined $Prevpos && defined $Prevdata
		&& $$tag[0] == $Prevpos
		&& length($$tag[1]) eq length($Prevdata))
	{	# -""- ---""---             .... ...1 = E-Bit: Last segment
		printf("-\"\"- ---\"\"--- %s%s\n", indent(), $$tag[2]);
		return;
	} elsif (defined $Prevpos && $Prevpos == $$tag[0])
	{	# Back in time.
		printf("------------- %s%s\n", indent(), $$tag[2]);
		return;
	}

	# Does the data fit on a single line?
	if (length($$tag[1]) < 8)
	{	# Less than 4 bytes.
		($Prevpos, $Prevdata) = ($$tag[0], $$tag[1]);
		printf("%.4x %8s %s%s\n", $Prevpos,
			'.' x (8-length($Prevdata)) . $Prevdata,
			indent(), $$tag[2]);
	} else
	{
		# First print 4 bytes of data with description,
		# then the rest of the data without description.
		($Prevpos, $Prevdata) = ($$tag[0], $$tag[1]);
		my ($pos, $data) = ($Prevpos, $Prevdata);
		my $data8 = substr($data, 0, 8, '');
		printf("%.4x %s %s%s\n", $pos, $data8,
			indent(), $$tag[2]);
		while (length($data) > 0)
		{
			$pos += 4;
			$data8 = substr($data, 0, 8, '');
			printf("%.4x %8s\n", $pos,
				'.' x (8-length($data8)) . $data8);
		}
	}
}

# Tag starting.  We expect this structure:
# <packet>
#   <proto>
#     <field>
#       <field>
#         ...
#       </field>
#     </field>
#   </proto>
#   <proto>
#     ...
#   </proto>
# </packet>
sub start
{
	my ($tag, $attrs) = @_;

	if ($tag eq "pdml")
	{	# <pdml> must be the outermost element.
		die unless !@Tags;
		push(@Tags, $tag);
		return;
	} elsif ($tag eq "packet")
	{	# <packet> must be contained within <pdml>.
		die unless @Tags == 1 && $Tags[0] eq "pdml";
		push(@Tags, $tag);
		return;
	} elsif ($tag eq "proto")
	{	# <protocol> must be contained within a <packet>.
		die unless @Tags > 2 || 
			(@Tags == 2
				&& $Tags[0] eq "pdml"
				&& $Tags[1] eq "packet");
		$Proto = $$attrs{'name'};
		if ($Proto eq "fake-field-wrapper")
		{	# NOP
		} elsif ($Proto eq "geninfo" || $Proto eq "frame")
		{	# ------------- Frame 1: 432 bytes on wire ...
			print "------------- ", $$attrs{'showname'};
		} else
		{	#        [0+20] Internet Protocol Version 4, ...
			print_header(
			[
				$$attrs{'pos'},
				$$attrs{'size'},
				$$attrs{'showname'},
			]);
		}

		push(@Tags, $tag);
		return;
	}

	# We must have a <field>.
	die $tag if $tag ne "field";
	die if @Tags < 3 || (!ref $Tags[-1] && $Tags[-1] ne "proto");

	if ($Proto eq "geninfo")
	{	# -------------     Arrival Time: Feb  6, 2014 ...
		push(@Tags, [ undef, undef,
			"$$attrs{'showname'}: $$attrs{'show'}" ]);
	} elsif ($Proto eq "frame")
	{	# -------------     Frame Number: 1
		push(@Tags, [ undef, undef, $$attrs{'showname'} ]);
	} elsif ($Proto eq "fake-field-wrapper")
	{
		if ($$attrs{'name'} eq "data.data")
		{
			push(@Tags,
			[
				$$attrs{'pos'},
				$$attrs{'value'},
			]);
			$Fields = 1;
			return;
		} else
		{
			push(@Tags, [ ]);
			$Fields = 0;
			return;
		}
	} elsif ($$attrs{'name'} eq "expert"
		|| $$attrs{'name'} eq "tcp.analysis")
	{
		push(@Tags, [ undef, undef, $$attrs{'showname'} ]);
	} elsif ($$attrs{'name'} =~ /^(?:expert|tcp\.analysis)\./)
	{
		push(@Tags, [ undef, undef, $$attrs{'showname'} ]);
	} else
	{	# Data of the <packet>.
		my ($size, $data);

		# Verify that we have a pos, and either we have size and value
		# or we don't have either.
		defined $$attrs{'pos'}
			or die;
		!defined ($size = $$attrs{'size'}) || $size >= 0
			or die;
		$data = $$attrs{'unmaskedvalue'} // $$attrs{'value'};
		!defined $data || !length($data) || $size
			or die;
		$size || !defined $data || !length($data)
			or die;

		if ($size > 0)
		{	# There's data.  Pad it as necessary up to $size.
			length($data) <= 2*$size
				or die;

			substr($data, 0, 0, '0')
				if length($data) % 2;
			substr($data, 0, 0, "00" x ($size - length($data)/2))
				if length($data) < 2*$size;
		}

		# Print this field when the element has finished.
		# This is so because we don't know yet whether it
		# has subfields.
		push(@Tags,
		[
			$$attrs{'pos'},
			$data,
			$$attrs{'showname'} // $$attrs{'show'},
		]);
	}
	$Fields++;
}

# An element has finished.
sub finish
{
	die if !@Tags;
	if (!$Fields)
	{	# We've already printed what's necessary.
		pop(@Tags);
		return;
	} elsif (!ref $Tags[-1])
	{
		die;
	}

	if (!$Fields)
	{	# NOP
	} elsif ($Fields == 1)
	{	# Just a regular field.
		my $tag = $Tags[-1];
		if (!defined $$tag[2])
		{
			$Preindent = -2;
			print_header([ $$tag[0], length($$tag[1])/2, "Data"]);
			$$tag[2] = "";
			$Preindent = 0;
		}
		print_field(pop(@Tags));
	} else
	{	# We have one or more subfield/protocol headers to print.
		do
		{
			my $tag = $Tags[-$Fields];
			$Preindent = -$Fields;
			if (defined $$tag[1])
			{
				print_header(
				[
					$$tag[0],
					length($$tag[1]) / 2,
					$$tag[2],
				]);
			} else
			{
				print_field($tag);
			}
		} while (--$Fields > 1);
		$Preindent = 0;
		print_field(pop(@Tags));
	}

	$Fields = 0;
}

# Ignition
sub run
{
	HTML::Parser->new(
		api_version	=> 3,
		xml_mode	=> 1,
		start_h		=> [ \&start, "tagname, attr" ],
		end_h		=> [ \&finish ])->parse_file(shift);
}

# Main starts here
$\ = "\n";
$Preindent = 0;
if (!@ARGV)
{
	run(*STDIN);
} elsif (@ARGV == 1)
{
	run(shift);
} elsif ($ARGV[-2] eq "-r")
{
	open(WS, '-|', qw(tshark -T pdml), @ARGV)
		or die;
	run(*WS);
} else
{
	die;
}

# vim: set foldmethod=marker foldmarker={{{,}}}:
# End of depcap.pl
