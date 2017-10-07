# enhanced argparse
import argparse
import re, socket

REVISION = "$Id: 98eb427eb996f3b615815ad626f718371b672eb6 $"

# Change some user-visible texts.
argparse_gettext = argparse._
def override_gettext(message):
	if message == "usage: ":
		message = message.capitalize()
	elif message == "optional arguments":
		message = "Options"
	elif message == "positional arguments":
		message = "Operands"
	elif message == "show this help message and exit":
		message = "Show this help."
	return argparse_gettext(message)
argparse._ = override_gettext

__all__ = [ "ArgumentParserExtra" ]
class ArgumentParserExtra(argparse.ArgumentParser):
	ArgumentTypeError = argparse.ArgumentTypeError

	class SubParsers(argparse._SubParsersAction):
		def add_parser(self, *args, **kwargs):
			# make the subcommand's 'description' the same as
			# its help (in the parent) unless specified otherwise
			if "help" in kwargs and "description" not in kwargs:
				kwargs["description"] = kwargs["help"]
			return super().add_parser(*args, **kwargs)
	class MandatorySubCommands(SubParsers):
		def __call__(self, parser, ns, values, *args, **kwargs):
			value = values[0]
			alternatives = self.choices.keys()
			candidates = parser.best_matches(value, alternatives)
			parser.subcommand_invoked = True

			if candidates is None:
				candidates, is_plural = \
					parser.multi_alternatives(
						alternatives)
				error = [ "subcommand" ]
				error.append('"' + value + '"')
				error.append("should be")
				if is_plural:
					error.append("either")
				error.append(candidates)
				raise argparse.ArgumentError(None,
					' '.join(error))
			elif isinstance(candidates, list):
				subparsers = list(map(
					lambda k: id(self.choices[k]),
					candidates))
				if not all(map(lambda k: k == subparsers[0],
						subparsers)):
					error = parser.multi_candidates(
						value, candidates)
					raise argparse.ArgumentError(None,
						"subcommand " + error)
				subparser_id = subparsers[0]
			else:
				subparser_id = id(self.choices[candidates])

			# values[0] := @canonical subcommand name
			for canonical, subparser in self.choices.items():
				if id(subparser) == subparser_id:
					values[0] = canonical
					break
			assert(subparser is not None)

			subcommands = getattr(parser, "subcommands", None)
			if subcommands is not None:
				# @parser.subcommands -> @subparser
				subcommands.append(values[0])
				subparser.subcommands = subcommands
			super().__call__(parser, ns, values, *args, **kwargs)
			if subcommands is not None:
				del subparser.subcommands
	class OptionalSubCommands(MandatorySubCommands):
		def __init__(self, *args, **kwargs):
			super().__init__(*args, **kwargs)
			self.nargs = '...'
		def __call__(self, parser, ns, values, *args, **kwargs):
			if not values:
				return
			super().__call__(parser, ns, values, *args, **kwargs)

	# don't set the value to False by default
	class StoreTrue(argparse._StoreConstAction):
		def __init__(self, option_strings, dest,
				default=None, required=False, help=None):
			super().__init__(
				option_strings=option_strings,
				dest=dest,
				const=True,
				default=default,
				required=required,
				help=help)

	# fix the case when the positional argument's action is "append"
	# and its nargs='*' or '?' (which makes little sense by the way).
	# if no positional argument is present the inherited _AppendAction
	# would take a copy of @self.default and append that to itself.
	# so if the default is ['pina'] @ns.dest would be set to ['pina',
	# ['pina']].  this also breaks some_of grouping if the positional
	# is involved.
	#
	# if @self.default wasn't set explicitly @ns.dest
	#		would become	should be
	# -- nargs='?'	[None]		None
	# -- nargs='*'	[[]]		[]
	class AppendAction(argparse._AppendAction):
		def __call__(self, parser, ns, values, *args, **kwargs):
			if id(values) != id(self.default) \
					or (self.default is None
						# and it's an optional arg
						and self.option_strings):
				super().__call__(parser, ns, values,
					*args, **kwargs)
			elif not hasattr(ns, self.dest):
				setattr(ns, self.dest, self.default
					if self.default is not None
					else [ ])

	class OnOff(argparse.Action):
		def __init__(self, *args, **kwargs):
			super().__init__(*args, **kwargs)
			self.nargs = 0

			if not self.option_strings:
				raise TypeError("positional argument "
						"cannot be on-off")

			long_opts = [ opt for opt in self.option_strings
					if opt.startswith("--") ]
			if not long_opts:
				raise TypeError("on-off type argument "
						"requires a long option name")

			for opt in long_opts:
				self.option_strings.append("--no-" + opt[2:])
		def __call__(self, parser, namespace, values, option_string,
				*args, **kwargs):
			setattr(namespace, self.dest,
				not option_string.startswith("--no-"))

	class Boolean(argparse.Action):
		on  = ("on",  "yes", "true",  "1")
		off = ("off", "no",  "false", "0")

		def __init__(self, *args, **kwargs):
			super().__init__(*args, **kwargs)
			self.nargs = '?'
			self.metavar = "on|off"
		def __call__(self, parser, namespace, values, *args, **kwargs):
			if self.default is not None and values is self.default:
				return
			elif values is None or values.lower() in self.on:
				setattr(namespace, self.dest, True)
			elif values.lower() in self.off:
				setattr(namespace, self.dest, False)
			else:
				raise argparse.ArgumentError(self,
					"boolean value expected")

	class Password(argparse.Action):
		def __init__(self, *args, **kwargs):
			super().__init__(*args, **kwargs)
			self.nargs = 0
		def __call__(self, parser, namespace, values, *args, **kwargs):
			import sys

			if not sys.stdin.isatty():
				sys.stdout.write("Password: ")
				sys.stdout.flush()
				password = sys.stdin.readline().rstrip('\n')
				print()
				setattr(namespace, self.dest, password)
				return

			import getpass

			try:
				password = getpass.getpass()
				pwdagain = getpass.getpass("Password again: ")
			except EOFError:
				raise argparse.ArgumentError(self,
					"interrupted")

			if password != pwdagain:
				raise argparse.ArgumentError(self,
					"passwords do not match")
			setattr(namespace, self.dest, password)

	class MyHelpFormatter(argparse.HelpFormatter):
		def add_text(self, text):
			if not isinstance(text, (tuple, list)):
				super().add_text(text)
				return
			for block in text:
				super().add_text(block)

		def _format_args(self, action, default_metavar):
			get_metavar = self._metavar_formatter(action,
					default_metavar)
			if action.nargs == argparse.ZERO_OR_MORE:
				return "[%s] ..." % get_metavar(1)
			elif action.nargs == argparse.ONE_OR_MORE:
				return "%s ..." % get_metavar(1)
			else:
				return super()._format_args(action,
							default_metavar)

	def __init__(self, *args, add_help=argparse.SUPPRESS,
			formatter_class=MyHelpFormatter, **kwargs):
		kwargs["add_help"] = add_help is True
		kwargs["formatter_class"] = formatter_class
		if "conflict_handler" not in kwargs:
			kwargs["conflict_handler"] = "resolve"

		super().__init__(*args, **kwargs)
		self.some_of_groups = [ ]
		self.register("action", "on-off",	self.OnOff)
		self.register("action", "boolean",	self.Boolean)
		self.register("action", "password",	self.Password)
		self.register("action", "store_true",	self.StoreTrue)
		self.register("action", "append",	self.AppendAction)
		self.register("action", "parsers",	self.SubParsers)

		if add_help == argparse.SUPPRESS:
			# add -h but omit it from the help
			self.add_argument(
				"-h", "--help", action='help',
				default=argparse.SUPPRESS,
				help=argparse.SUPPRESS)

	def add_subparsers(self, *args, **kwargs):
		optional = kwargs.pop("optional", None)
		if optional:
			kwargs["action"] = self.OptionalSubCommands
			if not "title" in kwargs:
				kwargs["title"] = "Optional subcommands"
		elif optional is False or "action" not in kwargs:
			kwargs["action"] = self.MandatorySubCommands
			if not "title" in kwargs:
				kwargs["title"] = "Possible subcommands"

		return super().add_subparsers(*args, **kwargs)

	def add_some_of_group(self, **kwargs):
		if "title" not in kwargs:
			kwargs["title"] = "Choose one or more from"
		group = self.add_argument_group(**kwargs)
		self.some_of_groups.append(group)
		return group

	rere = re.compile("^/(.*)/([a-z]*)$", 0)
	regexp_class = type(rere)

	@classmethod
	def mkre(self, regexp, throw=True):
		regstr, regexp = regexp, __class__.rere.match(regexp)
		if regexp is None:
			if not throw:
				return None
			raise TypeError(
				"\"%s\": not a regular expression" % regstr)

		flags = 0
		for flag in regexp.group(2):
			if flag == 'i':
				flags |= re.IGNORECASE
			else:
				raise TypeError("\"%s\": unknown flag '%c'"
					% (regexp.string, flag))

		return re.compile('^(?:' + regexp.group(1) + ')$', flags)

	@classmethod
	def match_regexp(self, value, regexp):
		match = regexp.search(value, 0)
		if not match:
			raise ValueError
		if match.lastindex is not None:
			return match.group(match.lastindex)
		else:
			return value
	argparse._ActionsContainer.mkre = mkre

	@classmethod
	def best_matches(self, value, alternatives):
		candidates = None
		for alternative in alternatives:
			if callable(alternative):
				try: return alternative(value)
				except ValueError: continue
				except self.ArgumentTypeError: continue
			elif isinstance(alternative, __class__.regexp_class):
				try: return __class__.match_regexp(value,
						alternative)
				except ValueError: continue
			elif not isinstance(alternative, str):
				try: converted = type(alternative)(value)
				except ValueError: continue
				if converted == alternative:
					return alternative
			elif not alternative.startswith(value):
				continue
			elif len(alternative) == len(value):
				return alternative
			elif not candidates:
				candidates = [ alternative ]
			else:
				candidates.append(alternative)
		if not value:
			return None
		return candidates

	@classmethod
	def multi_alternatives(self, alternatives):
		if not alternatives:
			return None, None

		doc = [ ]
		fixed = 0
		for alternative in alternatives:
			if isinstance(alternative, str):
				doc.insert(fixed, '"' + alternative + '"')
				fixed += 1
				continue
			elif isinstance(alternative, __class__.regexp_class):
				what = "proper string"
			elif callable(alternative):
				what = alternative.__name__
			else:
				doc.insert(fixed, str(alternative))
				fixed += 1
				continue

			try:	doc.index(what, fixed)
			except ValueError:
				doc.append(what)

		last = doc.pop()
		if doc:
			return "%s or %s" % (", ".join(doc), last), True
		else:
			return last, False

	@classmethod
	def multi_candidates(self, value, candidates):
		assert(isinstance(candidates, list))
		assert(len(candidates) > 1)

		last = candidates.pop()
		return "\"%s\" is ambiguous, did you mean %s or %s?" \
			% (value, ", ".join(candidates), last)

	@classmethod
	def multi(self, value, alternatives):
		candidates = self.best_matches(value, alternatives)
		if candidates is None:
			candidates, is_plural = self.multi_alternatives(
				alternatives)
			if candidates is None:
				raise argparse.ArgumentTypeError(
					"\"%s\": cannot accept any argument"
						% value)
			elif is_plural:
				error = "\"%s\" should be either %s"
			else:
				error = "\"%s\" should be %s"
			raise argparse.ArgumentTypeError(
				error % (value, candidates))
		elif not isinstance(candidates, list):
			return candidates
		elif len(candidates) == 1:
			return candidates[0]
		else:
			raise argparse.ArgumentTypeError(
				self.multi_candidates(value, candidates))

	def add_argument_mine(self, *args, **kwargs):
		# construct a suitable metavar
		if "metavar" not in kwargs:
			# find out whether action.__init__() accepts metavar
			action = kwargs.get("action")
			action = self._registry_get("action", action, action)
			init = action.__init__.__code__
			if init.co_flags & (0x04 | 0x08):
				# action.__init__() takes *args or **kwargs
				has_metavar = True
			else:	# does init() have metavar argument?
				try:	# throws ValueError if not found
					init.co_varnames.index("metavar",
						0, init.co_argcount)
				except ValueError:
					has_metavar = False
				else:
					has_metavar = True

			if not has_metavar:
				# action.__init__() doesn't accept metavar
				pass
			elif args[0][0] not in self.prefix_chars:
				# positional argument (doesn't start with '-')
				# metavar would be plain args[0]
				# make it uppercase
				metavar = args[0].upper()
				metavar = metavar.replace('_', '-')
				kwargs["metavar"] = metavar
			elif "dest" in kwargs:
				# optional argument
				# since metavar is missing it would be
				# constructed from dest
				metavar = args[0].upper()
				metavar = metavar.lstrip(metavar[0])
				kwargs["metavar"] = metavar

		if "regexp" in kwargs:
			kwargs["type"] = __class__.mkre(kwargs.pop("regexp"))
		regexp = kwargs.get("type")
		if isinstance(regexp, __class__.regexp_class):
			fun = lambda value: \
				__class__.match_regexp(value, regexp)
			metavar = kwargs.get("metavar")
			if isinstance(metavar, str):
				fun.__name__ = metavar
			else:
				fun.__name__ = "string"
			kwargs["type"] = fun

		alternatives = kwargs.pop("multi", None)
		if alternatives is not None:
			kwargs["type"] = lambda value: \
				__class__.multi(value, alternatives)
			if "help" not in kwargs:
				candidates, is_plural = \
					__class__.multi_alternatives(
						alternatives)
				if candidates is None:
					kwargs["help"] = argparse.SUPPRESS
				elif is_plural:
					kwargs["help"] = "Choose from %s." \
						% candidates
				else:
					kwargs["help"] = "Can only be %s." \
						% candidates

		if args[0][0] not in self.prefix_chars:
			# positional argument.  if it's nargs is '*'
			# and it doesn't have an explicit default,
			# argparse in python3.4 would wrongly consider
			# it required and show wrong usage text.
			if kwargs.get('nargs') == '*' \
					and 'default' not in kwargs:
				kwargs["default"] = [ ]

		return __class__.add_argument_orig(self, *args, **kwargs)
	add_argument_orig = argparse._ActionsContainer.add_argument
	argparse._ActionsContainer.add_argument = add_argument_mine

	def _check_value(self, action, value):
		if not isinstance(action, self.MandatorySubCommands):
			super()._check_value(action, value)

	def parse_known_args(self, args=None, namespace=None):
		# move the positional argument group to the end of the help
		if len(self._action_groups) > 1 \
			and self._action_groups[0] is self._positionals:
			self._action_groups.append(self._action_groups[0])
			del self._action_groups[0]

		self.subcommand_invoked = False
		ns, argv = super().parse_known_args(args, namespace)

		# verify that @some_of_groups has been invoked
		actions = list(action
			for group in self.some_of_groups
				for action in group._group_actions)
		if actions and all(map(lambda action:
				getattr(ns, action.dest) == action.default,
				actions)):
			what = [ ]
			for action in actions:
				if action.option_strings:
					what += action.option_strings
				elif action.metavar:
					what.append(action.metavar)
				else:
					what.append(action.dest)
			last = what.pop()
			self.error("either %s or %s is required" \
					% (", ".join(what), last) if what \
				else "%s is required" % last)

		# in python3.4 subcommands are not mandatory anymore
		if not self.subcommand_invoked and any(map(
				lambda cls: cls == self.MandatorySubCommands,
				map(type, self._actions))):
			self.error("subcommand required")

		return ns, argv

	def parse_args(self, *args, **kwargs):
		subcommands = kwargs.pop("subcommands", None)
		if subcommands is None:
			return super().parse_args(*args, **kwargs)
		self.subcommands = subcommands

		ns, unknown = self.parse_known_args(*args, **kwargs)
		if unknown:
			# find the most specific subcommand's @action
			# and print its help
			action = self
			for subcommand in subcommands:
				# @action <= the first _SubParsersAction
				# in @action._actions
				action = next(filter(lambda action:
					isinstance(action,
						argparse._SubParsersAction),
					action._actions))
				action = action.choices[subcommand]

			msg = "unrecognized arguments: " \
				if len(unknown) > 1 \
				else "unrecognized argument: "
			action.error(msg + ' '.join(unknown))

		del self.subcommands
		return ns

# a unique object to be used as default to indicate the absence of an argument
UNSET = [ ]
__all__.append("UNSET")

# type-conversion functions
anyint = re.compile("^0([0-9]+)$")
def integer(s):
	return int(anyint.sub("0o\\1", s), base=0)
__all__.append("integer")

def unsigned(s):
	n = integer(s)
	if n < 0: raise ValueError
	return n
__all__.append("unsigned")

def ufloat(s):
	n = float(s)
	if n < 0: raise ValueError
	return n
ufloat.__name__ = "unsigned float"
__all__.append("ufloat")

def ipv4_address(addr):
	try: socket.inet_pton(socket.AF_INET, addr)
	except socket.error: raise ValueError
	return addr
ipv4_address.__name__ = "IPv4 address"
__all__.append("ipv4_address")

def ip_address(addr):
	try:
		socket.inet_pton(
			socket.AF_INET6 if ':' in addr else socket.AF_INET,
			addr)
	except socket.error:
		raise ValueError
	return addr
ip_address.__name__ = "IP address"
__all__.append("ip_address")

def ip_prefix(prefix):
	addr = prefix.split('/', 1)
	netmask = addr[1] if len(addr) > 1 else None
	addr = addr[0]

	if ':' in addr:
		try: socket.inet_pton(socket.AF_INET6, addr)
		except socket.error: raise ValueError
		if netmask is not None and not 0 <= int(netmask) <= 128:
			raise ValueError
		return prefix

	ipv4_address(addr)
	if netmask is not None:
		try:
			netmask = int(netmask)
		except ValueError:
			ipv4_address(netmask)
		else:
			if not 0 <= netmask <= 32:
				raise ValueError
	return prefix
ip_prefix.__name__ = "IP/prefix"
__all__.append("ip_prefix")

invalid_hostname_char = re.compile("[^a-zA-Z0-9-]", 0)
def hostname(host):
	try:
		ip_address(host)
		return host
	except ValueError:
		pass

	domain = host[:-1] if host[-1] == '.' else host
	if len(domain) > 253:
		raise ValueError

	labels = domain.split('.')
	for label in labels:
		if not 0 < len(label) < 64:
			raise ValueError
		if label.startswith('-'):
			raise ValueError
		if label.endswith('-'):
			raise ValueError
		if invalid_hostname_char.search(label):
			raise ValueError

	if labels[-1].isdecimal():
		raise ValueError

	return host
__all__.append("hostname")

# the |. alternative is needed for error detection (m.groups()
# would return None, None)
hms_re = re.compile("(\d+(?:\.\d+)?)([wdhms])|.", re.IGNORECASE)
def hms(arg):
	if len(arg) == 0:
		raise argparse.ArgumentTypeError("required argument missing")

	sec = 0
	for m in hms_re.finditer(arg):
		n, d = m.groups()
		if n is None:
			raise argparse.ArgumentTypeError(
				"expected something like `4w3d12h30m15s'")
		n, d = float(n), d.lower()

		if d == 'w':
			sec += n * 7*24*60*60
		elif d == 'd':
			sec += n * 24*60*60
		elif d == 'h':
			sec += n * 60*60
		elif d == 'm':
			sec += n * 60
		else:
			sec += n
	return int(round(sec))
__all__.append("hms")

# take a YYYY-MM-DDTHH:MM:SSZ-like timestamp and return its distance
# from the epoch in milliseconds
re_timestamp_tail = re.compile(r"((?:\.(\d+))?Z?)$")
def timestamp(value):
	import time

	ms = 0
	first, T, second = value.partition(' ' if ' ' in value else 'T')
	if T:
		dte, tme = first, second
	elif '-' in first or '/' in first:
		dte = first
		tme = 0
	else:	# @dte := today 00:00:00
		tme = first
		dte = int(time.time())
		now = time.gmtime(dte)
		dte -= now.tm_hour*60*60 + now.tm_min*60 + now.tm_sec

	if isinstance(dte, str):
		# convert @dte to number of seconds since epoch
		if '/' in dte:
			# add the year in order to be able to check
			# the validity of the day of the month
			dte = "%d/%s" % (time.gmtime().tm_year, dte)
			fmt = "%Y/%m/%d"
		else:
			fmt = "%Y-%m-%d"

		try:
			dte = time.strptime(dte, fmt)
		except ValueError:
			raise ArgumentParserExtra.ArgumentTypeError(
				"specify date as YYYY-MM-DD or MM/DD")
		else:
			dte = int(time.mktime(dte))

	if isinstance(tme, str):
		# strip the 'Z' suffix from @tme and extract the subseconds
		m = re_timestamp_tail.search(tme)
		if m.start(1) != m.end(1):
			# there is a 'Z' suffix
			tme = tme[:m.start(1)]
			subsec = m.group(2)
			if subsec: # subseconds were specified
				# 0.500 -> 0.5
				subsec = subsec.rstrip('0')
			if subsec: # we have non-0 subseconds
				if len(subsec) > 3:
					# 0.1234s has sub-milliseconds
					# precision, which we cannot handle
					raise ArgumentParserExtra. \
						ArgumentTypeError(
							"cannot specify "
							"time with more "
							"than milliseconds "
							"precision")

				# 0.5s -> 500ms
				ms = int(subsec)
				for _ in range(0, len("999")-len(subsec)):
					ms *= 10

		# @tme := number of seconds since midnight
		fmt = "%H:%M" if tme.count(':') == 1 else "%H:%M:%S"
		try:
			tme = time.strptime(tme, fmt)
		except ValueError:
			raise ArgumentParserExtra.ArgumentTypeError(
				"specify time as HH:MM:SS or HH:MM")
		else:
			tme = tme.tm_hour*60*60 + tme.tm_min*60 + tme.tm_sec

	return (dte + tme)*1000 + ms
__all__.append("timestamp")

if __name__ == '__main__':
	WORDS = ("cohesion", "breed", "certified", "common",
		"consuetudinary", "bestow", "be seated", "condensation",
		"clustering", "briefed", "bromidic", "charge", "bevy",
		"base", "circumscript", "be pregnant", "bewhiskered",
		"company", "clamp", "candy", "condense", "chronic",
		"bracket", "blueprinted", "crusty", "concretion", "band",
		"clotting", "crystallize", "coherence", "coagulation",
		"corny", "be knocked up", "battery", "cabal", "cocked",
		"backing", "bend to", "constant", "cock", "cohort",
		"banal", "bunch", "conformation", "borne", "cinch",
		"compose", "cast", "category", "bullheaded", "crowd",
		"compaction", "conglomerate", "branch", "constitution",
		"bed", "chisel", "covey", "clinch", "conformable", "cramp",
		"comportment", "create", "clock", "clabber", "congealment",
		"belay", "cut to", "brigade", "crystallized", "brood",
		"certain", "calloused", "carve", "confirm", "concrete",
		"corps", "conventional", "caste", "congelation",
		"bulldogged", "cemented", "charmed circle", "cake",
		"chassis", "configuration", "coagulate", "cut-and-dried",
		"bend", "complement", "bear", "cover", "backward motion",
		"crusted", "console", "collection", "be-all", "cicatrize",
		"charted", "broadcast", "current", "carry", "barb",
		"build", "circumscribed", "contingent", "bulletheaded",
		"be-all and " "end-all", "cohesiveness", "customary",
		"curd", "calcified", "cut", "battalion", "congeal",
		"callous", "contrived", "couple", "conglomeration",
		"balky", "celestial navigation", "circle", "class",
		"clump", "bump", "case-hardened", "closed circle",
		"be gravid", "cluster", "beginning and end", "close",
		"cutting", "coordinate", "build in", "bearing", "curdle",
		"career", "conglobation", "clot", "bigoted", "copy",
		"clasp", "concourse", "conduce", "confirmed", "clinging",
		"cling", "cohere", "cleave", "block out", "burden with",
		"booted and spurred", "balking", "calculated", "be with "
		"child", "coterie", "consolidation", "cave in", "clique",
		"course", "cave", "crew", "cell", "character", "blood",
		"contribute", "bonded", "coached", "close up", "cling to",
		"camarilla", "conflux", "bet", "backflowing", "crosscurrent",
		"carry young", "bent", "cement", "commonplace", "bias",
		"bearings", "confluence", "cadre", "block", "climbing",
		"clan", "cabinet", "body")

	def link_local_address(addr):
		if ':' in addr:
			for n in addr.split(':'):
				if not 0 <= int(n, base=16) <= 255:
					raise ValueError
			return addr
		else:
			return ipv4_address(addr)
	link_local_address.__name__ = "link-local address"

	parser = ArgumentParserExtra()

	parser.add_argument("--foo", type=integer)
	parser.add_argument("--bar", type=unsigned)
	parser.add_argument("--baz", type=float)
	parser.add_argument("--bazz", action="on-off")
	parser.add_argument("--buzz", action="boolean")
	parser.add_argument("--buzi", multi=WORDS + (10, 20, 30))
	parser.add_argument("--buza", multi=(unsigned, "forever"))
	parser.add_argument("--biza", multi=(ip_address, unsigned), nargs=2)

	sub = parser.add_subparsers(optional=False)
	sub.add_parser("alpha").add_argument("--boo", type=ipv4_address)
	sub.add_parser("gamma").add_argument("--poo", type=ip_address)
	sub.add_parser("delta").add_argument("--too", type=ip_prefix)
	sub.add_parser("sigma").add_argument("--moo", type=hostname)
	sub.add_parser("summa").add_argument("--goo", type=link_local_address)

	sub = sub.add_parser("epsilon", aliases=("kaksi", "katu"))
	sub = sub.add_subparsers().add_parser("omega")
	sub = sub.add_some_of_group()
	sub.add_argument("--akarmi", metavar="FWMARK",
		regexp="/(?:0x[0-9a-f]+|[0-9]+)"
			"(?:/(?:0x[0-9a-f]+|[0-9]+))?/i")
	sub.add_argument("--valami", type=sub.mkre("/[A-Z]+/"))
	sub.add_argument("--barmi", type=re.compile("([AEIOU]+)"))
	sub.add_argument("--semmi", regexp="/[^/]+/", action="append")
	sub.add_argument("--nemtommi", action="store_true")
	sub.add_argument("hi_hi_hi", nargs='*',
		action="append", default=[["pina"]],
		multi=(sub.mkre("/[01]+/"), unsigned, "haha"))

	subcommands = [ ]
	print(parser.parse_args(subcommands=subcommands))
	print(subcommands)

# End of arpextra.py
