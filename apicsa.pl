#!/usr/bin/perl -w
#
# apicsa.pl -- mass-upload photos to Google Picasa
#
# Given a google account, apicsa.pl will upload the specified files
# and directories to Picasa non-interactively.
#
# Synopsis:
#	apicsa.pl [-u <user>] {-a <album> | <thing-to-upload>} ...
#
# Where:
# <user> is your google account name, either with or without domain part.
# If no domain supplied, @gmail.com is assumed.
#
# <thing-to-upload> is either a file or a firectory.  If it's an individual
# file, it is uploaded to <album>.  If no <album> has been specified Picasa's
# drop box is assumed.  Subsequent <album>s override each other.
#
# If <thing-to-upload> is a directory then its direct contents are uploaded
# to the album whose name is derived from the directory's base name.  If the
# base name turns out to be either '.' or '..' then <album> is assumed.
#
# Albums are created if and only if they don't exist yet.  This case their
# visibility is left private.  apicsa.pl attemts to provide a sane photo
# summary (title) by mungling the base file name.
#
# Most errors are not fatal, but may cause to skip the uploading of photos.
# If anything unexpected has happened during operation, the program will
# exit with an error at the end.
#
# Examples:
#
# apicsa.pl -u pista /tmp/Party_photos_2008
#	Uploads photos from directory /tmp/Party_photos_2008
#	into pista's 'Party photos 2008' album, creating it
#	if doesn't exist.
#
# apicsa.pl -u pista -a 'Spriccparty tonight' /tmp/img/*
#	Uploads all photos in /tmp/img into the 'Spriccparty tonight' album.
#

# Modules
use strict;
use Encode qw();
use LWP;
use HTTP::Status;
use IPC::Open3;

# Service URL:s.
use constant AUTH	=> 'https://www.google.com/accounts/ClientLogin';
use constant PICASA	=> 'http://picasaweb.google.com/data/feed/api/user/default';

# Private variables
my $Had_errors = 0;
my ($File_in, $File_ex);

# Program code
# Inform the user about an error and set the flag that causes
# the program to exit with error status set.
sub error
{	# Return undef to make it convenient to use it in a return statement.
	print STDERR "apicsa: ", @_;
	$Had_errors = 1;
	return undef;
}

# Returns the MIME-type of $fname.
sub mime
{
	my $fname = shift;
	my $mime;

	# Spawm a file(1) to do the hard work if we haven't.
	defined $File_ex
		or defined open3($File_ex, $File_in, undef,
			qw(file -n --mime --brief --file -))
		or die "file";

	print $File_ex $fname;
	chomp($mime = <$File_in>);
	$mime =~ s/^ERROR: //
		and die $mime;
	return $mime;
}

# Returns the contents of $fname as a whole.
sub slurp
{
	my $fname = shift;
	local *FNAME;

	local $/ = undef;
	open(FNAME, '<', $fname)
		or die "$fname: $!";
	return <FNAME>;
}

sub encode
{
	my $str = shift;

	$str = Encode::encode('UTF8', $str);
	$str =~ s/&/&amp;/g;
	$str =~ s/</&lt;/g;
	$str =~ s/>/&gt;/g;
	return $str;
}

# Asks the user $prompt and returns the answer, not echoing it back
# if necessary.
sub ask
{
	my ($prompt, $noecho) = @_;
	my $ans;

	local $\ = "";
	print $prompt, ": ";

	if ($noecho)
	{	# Restore echo if interrupted meanwhile.
		local $SIG{'INT'} = local $SIG{'TERM'} = sub
		{
			system(qw(stty echo));
			exit 1;
		};
		system(qw(stty -echo));
	}

	$ans = <STDIN>;
	if ($noecho)
	{
		print "\n";
		system(qw(stty  echo));
	}

	chomp($ans) if defined $ans;
	return $ans;
}

# Overwrite $argr with the argument of switch $what and return it
# if the next thing in @ARGV is $what.  Otherwise returns undef.
sub nextarg
{
	my ($what, $argr) = @_;

	@ARGV && $ARGV[0] eq $what
		or return undef;
	shift(@ARGV);
	@ARGV > 0
		or exit !error("option $what requires an argument.");
	return $$argr = shift(@ARGV);
}

# Acquires the authorization token from google and adds it to $ua
# so further requests will automatically use it.
sub login
{
	my ($ua, $user, $pass) = @_;
	my ($url, $rep, $auth);

	# The $user must be fully qualified.
	$user =~ /@/ or $user .= '@gmail.com';
	$url = URI->new(AUTH);
	$url->query_form(service => 'lh2',
		Email => $user, Passwd => $pass,
		accountType => 'GOOGLE', source => 'megsutlek');

	$rep = $ua->post($url);
	$rep->code() == RC_OK
		or return error('turn away, impostor!');
	($auth) = $rep->content() =~ /^Auth=(.*)$/m;
	defined $auth
		or return error('google is nuts');

	$ua->default_headers->push_header(
		Authorization => "GoogleLogin auth=$auth");
	return 1;
}

# Returns list of available albums as a $title => $albumid hash.
# If mor ethan one albums have the same title, only one of them
# will be listed.
sub lsalbums
{
	my $ua = shift;
	my ($rep, %list, $id, $title);

	$rep = $ua->get(PICASA);
	$rep->is_success()
		or return error("failed to read the list of albums: ",
			$rep->message());

	# The response is a series of <entry>:es in a <feed>,
	# each having an <id> and a <title>.  The albumid is
	# contsined in the <id> as a long numeric string.
	$rep->content() =~ m!
	<entry>
	(?:
		 <id>.*?/albumid/(\d+)(?{$id=$^N})\s*</id>
		|<title.*?>\s*(.*?)(?{$title=$^N})\s*</title>
		|</entry>(?{$list{$title} = $id; $id=$title=undef})
		|.)*
	!gsx;

	return \%list;
}

# Creates a new album with $title and returns its albumid.
sub mkalbum
{
	my ($ua, $title) = @_;
	my ($req, $rep);

	$title = encode($title);
	$req = HTTP::Request->new(POST => PICASA);
	$req->content(<< "HUFFLEPUFF");
<entry xmlns="http://www.w3.org/2005/Atom">
  <title type="text">$title</title>
  <category
    scheme="http://schemas.google.com/g/2005#kind"
    term="http://schemas.google.com/photos/2007#album"/>
</entry>
HUFFLEPUFF

	$rep = $ua->request($req);
	$rep->code() == RC_CREATED
		or return error("oops, couldn't create album: ",
			$rep->message());

	$rep->content() =~ m!/albumid/(\d+)\s*</id>!i
		or return error("google lost sanity points");
	return $1;
}

# Uploads $fname into $album, adding a suitable <title> and <summary>.
sub upload
{
	my ($ua, $album, $fname) = @_;
	my ($title, $fbase, $req, $meta, $file, $rep);

	# The basename of $fname will tbe the <title>.
	$fname =~ m!([^/]*)$ !x;
	$fbase  = $1;

	# And the <summary> is its human-readable equivalent.
	$title = do { use locale; ucfirst($fbase) };
	$title =~ s/\.\w{1,4}$//;
	$title =~ tr/_/ /;

	$fbase = encode($fbase);
	$title = encode($title);

	$req = HTTP::Request->new(
		POST => join('/', PICASA, 'albumid', $album));
	$req->content('Media multipart posting');
	
	$meta = HTTP::Message->new();
	$meta->header('Content-Type' => 'application/atom+xml');
	$meta->content(<< "GRYFFINDOR");
<entry xmlns="http://www.w3.org/2005/Atom">
  <title type="text">$fbase</title>
  <summary type="text">$title</summary>
  <category
    scheme="http://schemas.google.com/g/2005#kind"
    term="http://schemas.google.com/photos/2007#photo"/>
</entry>
GRYFFINDOR
	chomp(${ $meta->content_ref() });

	$file = HTTP::Message->new();
	eval
	{
		$file->header('Content-Type' => mime($fname));
		$file->content(slurp($fname));
	};
	return error($@) if $@;

	$req->parts($meta, $file);
	$req->header('Content-Type'	=> 'multipart/related');
	$req->header('Content-Length'	=> length($req->content()));
	$req->header('MIME-Version'	=> '1.0');

	$rep = $ua->request($req);
	$rep->code() == RC_CREATED
		or error("failed to upload $fname: ", $rep->message());
}

# Main starts here
my ($user, $pass, $ua);
my ($albums, $defalname);

# Need help?
$\ = "\n";
exit !error("usage: $0 [-u <user>] {-a <album>|<file>|<directory>}...")
	if !@ARGV;

# Get $user, $pass.
$user = nextarg('-u', \$user)
	or $user = ask('google user');
$pass = ask('password', 1);
defined $user && defined $pass
	or exit;

# Set up the HTTP UserAgent.
$ua = LWP::UserAgent->new();
$ua->default_headers->push_header(
	'Content-Type' => 'application/atom+xml');
login($ua, $user, $pass)
	or exit 1;

# Get the list of albums to not recreate albums that already exist.
defined ($albums = lsalbums($ua))
	or exit 1;
$$albums{'default'} = 'default';
$$albums{'dropbox'} = 'default';
$defalname = 'dropbox';

# Run
while (@ARGV)
{
	my $thing;

	# Process -a, but don't create the album just yet.
	next if defined nextarg('-a', \$defalname);

	$thing = shift;
	if	(-d $thing)
	{
		my $alname;

		# Convert the directory name to a human-readable album name.
		$alname = $thing;
		$alname =~ s!/+!!;
		$alname =~ s!^.*/+!!;
		$alname = do { use locale; ucfirst($alname) };
		$alname =~ tr/_/ /;
		$alname eq '.' || $alname eq '..'
			and $alname = $defalname;

		# Create $alname if it doesn't exist.
		print "Processing $thing ($alname)...";
		if (!defined $$albums{$alname})
		{
			print "\tCreating album...";
			$$albums{$alname} = mkalbum($ua, $alname);
			if (!defined $$albums{$alname})
			{
				error("skipping directory.");
				delete $$albums{$alname};
				next;
			}
		}

		foreach (<$thing/*>)
		{
			print "\tUploading $_...";
			upload($ua, $$albums{$alname}, $_);
		}
	} elsif (-f $thing)
	{
		# Upload to $defalname, creating it if it doesn't exist.
		if (!defined $$albums{$defalname})
		{
			print "Creating $defalname...";
			$$albums{$defalname} = mkalbum($ua, $defalname);
			if (!defined $$albums{$defalname})
			{
				error("skipping $thing.");
				delete $$albums{$defalname};
				next;
			}
		}

		print "$defalname: uploading $thing...";
		upload($ua, $$albums{$defalname}, $thing);
	} elsif (-e $thing)
	{
		error("$thing is neither a file nor a firectory, skipping.");
	} else
	{
		error("$thing not found.");
	}
}

# Done
exit $Had_errors;

# End of apicsa.pl
