/*
 * libartem.c -- LD_PRELOAD-able embedded syslog {{{
 *
 * In summary:
 * -- log to your terminal from your daemon
 * -- or get every daemon's logs in its own file
 * -- or discard the logs entirely, not writing
 *    or sending anything anywhere
 * -- covers stdio, g_log() and g_print*()
 *
 * Applications in maemo use g_log() and its higher level faces:
 * g_debug(), g_warning() and so on.  Under normal operation we'll
 * consider them daemons, because when they're started they don't
 * have a terminal to log onto.  Nevertheless, when you're developing
 * or bugfixing you want to log in many different places.  maemo's
 * modified glib sends all log messages to syslog, which everyone
 * hates.  Earlier it also requested that those messages be printed
 * on the terminal as well.  Recently libc has been crippled not to
 * honor this request, so all messages end up in syslog, and only
 * there -- if your image happens to have it at all.
 *
 * This library helps you to skip syslog entirely.  It provides you
 * application-level control where to log what or what not to log
 * at all.  It not only can redirect g_log() output to your desired
 * place but also stdout and stderr, making it usable in environments
 * which don't use glib.  You can link with this library in compile
 * time, or you can LD_PRELOAD it, or you can drop it in ld.so.preload
 * to achieve system-wide effect and benefits.
 *
 * You can configure libartem by creating a file system hierarchy in
 * CONFIG_DIR.  In the hierarchy symbolic links point to the destinations
 * of a particular logging channel (stdout, g_log() etc).  An example
 * may look like this:
 *
 * ./hildon-desktop/output		-> /var/tmp/ide
 *	# If hildon-desktop is started from the command line redirect
 *	# all its output to /var/tmp/ide.
 * ./hildon-desktop/daemon		-> .
 *	# Do that even when it's started by upstart.
 * ./osso_lmarbles/daemon/output	-> /dev/pts/0
 *	# Redirect Marbles' all output to the first pseudo terminal,
 *	# which is usually your first ssh client.  Make sure the
 *	# target file is writable by "user".  We're not interested
 *	# in non-daemonic Marbles because is quite difficult to
 *	# start it from the command line.  Also note that Marbles
 *	# is not a GTK application, but stderr will be useful for
 *	# us as well.
 * ./maemoblocks			-> osso_lmarbles
 *	# Do exactly the same to Blocks, another cool game we love
 *	# to debug.
 * ./DAEMONS/stderr			-> /var/tmp/siras/
 *	# This tells libartem to create the directory "/var/tmp/siras"
 *	# and redirect all daemon's stderr to its own file there.
 *
 * In general there are four types of directories in CONFIG_DIR.
 * All directories contain files (most likely symbolic links)
 * describing where to send the output of stdout, stderr etc.
 * -- ./<app>:		Applies to all programs started from the command line
 *  			whose basename(argv[0]) is <app>.  If you left some
 *  			destinations unspecified they will be searched for
 *  			one level upwards.  This directory may be a symlink
 *  			itself if you want the same treatment for two <app>s.
 * -- ./<app>/daemon:	Applies to <app>s started during system boot.
 *			Missing destinations are looked up in ./DAEMONS.
 *			You may want to symlink this directory to ".",
 *			so app's output will be the same whether it's
 *			a daemon or not.
 * -- .			This is CONFIG_DIR and it contains the destinations
 *			of output channels not specified on <app> level.
 * -- ./DAEMONS:	Like <app>/daemon.
 *
 * In the directories libartem search for these files:
 * -- stdout:		Telling where printf() and g_print() should go.
 *			If not specified anywhere for non-daemons it
 *			defaults to logging on libc's stdout.  For daemons
 *			the default is to not alter the output channels,
 *			so they will be essentially lost.
 * -- stderr:		For stderr, g_printerr() and g_log().
 *			Like above, non-daemons fall back to using libc's
 *			stderr, while daemons output is not altered,
 *			meaning that g_log() will continue to go to syslog.
 * -- output:		Checked when either `stdout' or `stderr'
 *			specification is missing.  Can be used
 *			to specify them together.
 * -- screwit:		If this file exists artem will forcefulli override
 *			application's logging settings.  This is useful if
 *			the program installs an empty log handler to appear
 *			quiet.  Overriding it is rather intrusive but can
 *			be useful.  Only effective if artem was built with
 *			CONFIG_PEDANTIC.  Also, you can set $ARTEM_SCREWIT
 *			to non-zero to achieve the same effect without
 *			configuration.
 *
 * -- existing file or symlink to an existing file:
 *    The file is opened and new output is appended.
 * -- symlink to /dev/null:
 *    This is treated as a special case (even if /dev/null doesn't exist
 *    or it's not the null device, but your usecase would be interesting).
 *    Output is not written anywhere, but silently discarded, without a
 *    trip to kernel space.
 * -- symlink to "/foo/bar/baz" where "/foo/bar" exist but "baz" doesn't:
 *    "baz" is created and opened as if it existed.
 * -- directory or symlink to an existing directory:
 *    Output will be appended to <dir>/<app>.  This is how to sort daemon's
 *    logs to their own files.
 * -- symlink to "/foo/bar/baz/" (note the trailing slash)
 *    where "/foo/bar" exist but "baz" doesn't:
 *    "baz" is created as a directory then output is redirected to
 *    "/foo/bar/baz/<app>".  This is useful when "/foo/bar" is cleaned up
 *    during system boot, like "/var/tmp".
 * -- symlink to LEAVEALONE:
 *    Don't alter the relevant output channels in any way.
 *    Useful to force something not to fall back to libartem's default.
 *
 * If you specified redirection for an output but the library failed
 * to open the destination it won't be redirected anywhere.
 *
 * At program startup libartem doesn't redirect anything yet, but waits
 * until you use any of the output channels (ie. log/print something).
 * This is so to avoid creation empty output files (if your program is
 * silent) and it also makes us maemo-launcher-friendly, provided that
 * anything can be friend with it.
 *
 * This is not a hacker tool.  If your program redirects output on its own
 * (ie. setting $stdout to something) libartem will try not to interfere
 * See also CONFIG_PEDANTIC.  If you don't like it don't file a bug.
 * }}}
 */

/* For $program_invocation_short_name and fopencookie(). */
#define _GNU_SOURCE

/*
 * Define it to make the library more seamless if the application set
 * its own default g_log() handler at the cost of more startup overhead
 * and a link to -ldl.  Also if you link your application with artem
 * make sure it comes earlier in the linking order than glib.
 * (Provided that you're interested in glib in the first place.)
 */
//#define CONFIG_PEDANTIC

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <string.h>
#include <stdio.h>
#ifdef CONFIG_PEDANTIC
# include <dlfcn.h>
#endif

#include <sys/stat.h>

#include <glib.h>

/* Standard definitions */
/* The root directory of our configuration */
#define CONFIG_DIR			"/etc/artem"

/* If we don't override g_log_set_default_handler() we can use glib's. */
#ifndef CONFIG_PEDANTIC
# define real_g_log_set_default_handler	g_log_set_default_handler
#endif

/* Macros */
#if 0
# define DEBUG(str)			\
	 write(STDERR_FILENO, str "\n", sizeof(str))
#else
# define DEBUG(str)			/* NOP */
#endif

/* Type definitions {{{ */
/* Information about an output stream that needs to be in global scope. */
struct artem_st
{
	/*
	 * $original:	libc's original $stdout or $stderr
	 * $probe:	the stream we installed in place of $original
	 *		to wake up when up print something on it
	 *		for the first time
	 * $redirected:	the stream our configuration wants this outputto go to
	 */
	FILE *original, *probe, *redirected;
};

/* Information about an output stream that we only need in setup() time. */
struct artem_state_st
{
	/*
	 * NONE:	we haven't figured out
	 * REDIR:	use $files->redirected,
	 *		$files->redirected is meaningful
	 * ERROR:	wanted to redirect, but couldn't create
	 * 		the output file; $error contains the $errno
	 * DEVNULL:	attempt not to write() anywhere,
	 *		$files->redirected is a special FILE stream
	 * LEAVEALONE:	don't attempt to redirect this output stream
	 */
	enum
	{
		NONE,
		REDIR,
		ERROR,
		DEVNULL,
		LEAVEALONE,
	} state;

	int error;
	struct artem_st *files;

	/* Can we override the application programmer's choice of
	 * GPrint_handler, GPrintErr_handler or GLog_handler? */
	int screwit;
};
/* Type definitions }}} */

/* Function prototypes */
static void probe_gprint(char const *str);
static void probe_gprinterr(char const *str);
static void probe_glog(gchar const *domain, GLogLevelFlags level,
	gchar const *message, gpointer unused);

/* Private variables {{{ */
/*
 * $Stdout: where $stdout and g_print() should go
 * $Stderr: where $stderr, g_printerr() and the default g_log() should go
 */
static struct artem_st Stdout, Stderr;

/*
 * The current g_print(), g_printerr() and default g_log() handlers.
 * Initially they are set to glib's defaults, but setup() may change them.
 */
static GPrintFunc GPrint_handler;
static GPrintFunc GPrintErr_handler;
static GLogFunc GLog_handler;
static gpointer GLog_handler_userdata;

#ifdef CONFIG_PEDANTIC
/* glib's own g_log_set_default_handler(), set by init() */
static GLogFunc (*real_g_log_set_default_handler)(GLogFunc, gpointer);
#endif
/* }}} */

/* Program code */
/* Our output handlers {{{ */
/* stdio write() handler */
static ssize_t write_to_devnull(void *unused, char const *buf, size_t sbuf)
{	/* NOP */
	return sbuf;
}

/* GPrintFunc:s {{{ */
static void gprint_to_stdout(char const *str)
{
	fputs(str, Stdout.redirected);
}

static void gprint_to_stderr(char const *str)
{
	fputs(str, Stderr.redirected);
}

static void gprint_to_original_stdout(char const *str)
{
	fputs(str, Stdout.original);
}

static void gprint_to_original_stderr(char const *str)
{
	fputs(str, Stderr.original);
}

static void gprint_to_devnull(char const *str)
{
	/* NOP */
}
/* }}} */

/* GLogFunc:s {{{ */
static void glog_to_stderr(gchar const *domain, GLogLevelFlags level,
	gchar const *message, gpointer unused)
{
	size_t lmessage;
	char const *levelstr;

	if (level & G_LOG_LEVEL_ERROR)
		levelstr = "ERROR";
	else if (level & G_LOG_LEVEL_CRITICAL)
		levelstr = "CRITICAL";
	else if (level & G_LOG_LEVEL_WARNING)
		levelstr = "WARNING";
	else if (level & G_LOG_LEVEL_MESSAGE)
		levelstr = "MESSAGE";
	else if (level & G_LOG_LEVEL_INFO)
		levelstr = "INFO";
	else /* level & G_LOG_LEVEL_DEBUG */
		levelstr = "DEBUG";

	/* Trim the trailing newline if there's one. */
	lmessage = strlen(message);
	if (lmessage > 0 && message[lmessage-1] == '\n')
		lmessage--;
	fprintf(Stderr.redirected, "%s[%d]: %s %s - %.*s\n",
		program_invocation_short_name, getpid(), levelstr,
	       	domain ? : "default", lmessage, message);
}

static void glog_to_devnull(gchar const *domain, GLogLevelFlags level,
	gchar const *message, gpointer unused)
{
	/* NOP */
}
/* }}} */
/* Output handlers }}} */

/* Finding the destinations {{{ */
/* Returns an stdio FILE which doesn't write() at all. */
static FILE *open_devnull(void)
{
	static FILE *devnull;
	static cookie_io_functions_t ops = { .write = write_to_devnull };

	if (!devnull)
		devnull = fopencookie(NULL, "w", ops);
	return devnull;
} /* open_devnull */

/*
 * Construct a pathname from the arguments and considers it as an output
 * destination.  Unless the path doesn't exist at all it fills out $atm.
 * Otherwise it handlers special cases and creates the path's containing
 * directory if necessary.  Returns whether the caller needs to consult
 * the next configuration for this output stream.
 */
static int try(struct artem_state_st *atm, char const *fmt, ...)
{
	FILE *st;
	va_list args;
	int lfullpath;
	char path[64], fullpath[128];

	/* Construct the $path. */
	va_start(args, fmt);
	vsnprintf(path, sizeof(path), fmt, args);
	va_end(args);

	/* What's $path?  Handle special cases. */
	if ((lfullpath = readlink(path, fullpath, sizeof(fullpath)-1)) >= 0)
	{	/* $path is a symlink */
		fullpath[lfullpath] = '\0';
		if (!strcmp(fullpath, "LEAVEALONE"))
		{
			atm->state = LEAVEALONE;
			return 1;
		} else if (!strcmp(fullpath, "/dev/null"))
		{	/* optimize */
			atm->state = DEVNULL;
			atm->files->redirected = open_devnull();
			return 1;
		}
	} else
	{
		if (errno == ENOENT)
			/* $path doesn't exist, try another one */
			return 0;
		if (errno != EINVAL)
			goto error;
		/* $path exists and is not a symlink */
	}

	/* $path exists and is either a symlink or not,
	 * try creating it as a file first. */
	if ((st = fopen(path, "a")) != NULL)
		goto ok;
	if (errno != EISDIR)
		goto error;

	/* $path is either a directory or a symlink pointing to a
	 * directory or points to "foo/bar/" where "foo" exists,
	 * but "bar" doesn't. */
	if (lfullpath >= 0)
	{
		/* $path must be a directory but it may not exist yet.
		 * Try creating it with the readlink()ed path because
		 * mkdir() doesn't create symlink destinations. */
		if (mkdir(fullpath, 0777) < 0 && errno != EEXIST)
			goto error;
	} else
		/* $path exists and is a directory */;

	/* $path exists and is a directory,
	 * try creating $path/$program_name */
	snprintf(fullpath, sizeof(fullpath), "%s/%s",
		path, program_invocation_short_name);
	if (!(st = fopen(fullpath, "a")))
		goto error;

ok:	/* Success, set line-buffering. */
	atm->state = REDIR;
	setvbuf(st, NULL, _IOLBF, 0);
	atm->files->redirected = st;
	return 1;

error:	/* Save $errno for further reference. */
	atm->state = ERROR;
	atm->error = errno;
	return 1;
} /* try */
/* Destinations }}} */

/* Configuration {{{ */
/* Returns whether the program has a controlling terminal.
 * If not then it must have been started as a service on boot.
 * We redirect logs to different paths if the program is a damon. */
static int isdaemon(void)
{
	guint tty;
	FILE *procst;

	/* Find it out from /proc. */
	if (!(procst = fopen("/proc/self/stat", "r")))
		return 0;	// wtf
	fscanf(procst,  "%*u "  // pid
			"%*s "  // comm
			"%*c "  // state
			"%*u "  // ppid
			"%*u "  // pgrp
			"%*u "  // session
			"%u ",  // tty_nr
			&tty);
	fclose(procst);
	return !tty;
} /* isdaemon */

/* Does $fmt exist in the file system? */
static int __attribute__((unused)) exists(char const *fmt, ...)
{
	va_list args;
	char path[64];

	va_start(args, fmt);
	vsnprintf(path, sizeof(path), fmt, args);
	va_end(args);
	return access(path, F_OK) == 0;
} /* exists */

/* Screw the application programmer's logging decision? */
static int screwit(int daemon)
{
#ifdef CONFIG_PEDANTIC
	char const *env;

	/* Check the environment first, then the config files. */
	if ((env = getenv("ARTEM_SCREWIT")) && atoi(env))
		return 1;
	if (daemon < 0)
		daemon = isdaemon();
	if (daemon)
		return exists("%s/%s/%s/%s", CONFIG_DIR,
				program_invocation_short_name,
				"daemon", "screwit")
			|| exists("%s/%s/%s", CONFIG_DIR,
				"DAEMONS", "screwit");
	else
		return exists("%s/%s/%s", CONFIG_DIR,
				program_invocation_short_name, "screwit")
			|| exists("%s/%s", CONFIG_DIR, "screwit");
#else /* ! CONFIG_PEDANTIC */
	return 0;
#endif
} /* screwit */

/* Configure $atm by looking at the application-specific
 * and the global configuration. */
static int config1(struct artem_state_st *atm,
	char const *root_subdir, char const *prog_subdir, char const *file)
{
	return try(atm, "%s/%s/%s/%s", CONFIG_DIR,
			program_invocation_short_name, prog_subdir, file)
		|| try(atm, "%s/%s/%s", CONFIG_DIR, root_subdir, file);
} /* config1 */

/* Configure both $Stdout and $Stderr according to the
 * daemoned-ness-dependant configuration of the application. */
static void config2(struct artem_state_st *out, struct artem_state_st *err,
	char const *root_subdir, char const *prog_subdir)
{
	/* Try <dir>/stdout then <dir>/output.  If the former fails
	 * but the latter succeeds then use it for both. */
	if (!config1(out, root_subdir, prog_subdir, "stdout")
		&& config1(out, root_subdir, prog_subdir, "output"))
	{
		err->state = out->state;
		err->error = out->error;
		err->files->redirected = out->files->redirected;
		return;
	}

	/* Try <dir>/stderr then if <dir>/output if it hasn't been. */
	if (!config1(err, root_subdir, prog_subdir, "stderr")
			&& out->state != NONE)
		config1(err, root_subdir, prog_subdir, "output");
} /* config2 */

/* Check out the configuration, determine and open the output destinations,
 * and fill out $out and $err. */
static void config(struct artem_state_st *out, struct artem_state_st *err)
{
	out->files = &Stdout;
	err->files = &Stderr;
	out->state = err->state = NONE;

	if (isdaemon())
	{	/* If nothing is specified, don't touch a daemon's output. */
		config2(out, err, "DAEMONS", "daemon");
		if (out->state == NONE)
			out->state = LEAVEALONE;
		if (err->state == NONE)
			err->state = LEAVEALONE;

		out->screwit = err->screwit = screwit(1);
	} else
	{	/* By default allow non-daemons to use the terminal. */
		config2(out, err, ".", ".");
		if (out->state == NONE)
		{
			out->state = REDIR;
			out->files->redirected = out->files->original;
		}
		if (err->state == NONE)
		{
			err->state = REDIR;
			err->files->redirected = err->files->original;
		}

		out->screwit = err->screwit = screwit(0);
	} /* if */
} /* config */
/* Configuration }}} */

/* Setup {{{ */
static int redirected(int fd)
{
	struct stat sbuf;

	return fstat(fd, &sbuf) == 0 && !S_ISCHR(sbuf.st_mode);
}

/* Figure out the final $stdout and GPrint_handler. */
static void setup_stdout(struct artem_state_st *atm)
{
	FILE *dst;
	GPrintFunc prev;

	/* Make $stdout the real $stdout if the program is redirected
	 * (possibly by the user from the command line). */
	if ((atm->state == REDIR || atm->state == DEVNULL)
			&& redirected(STDOUT_FILENO))
		atm->files->redirected = atm->files->original;

	if (atm->state == REDIR)
	{
		GPrint_handler = gprint_to_stdout;
		dst = atm->files->redirected;
	} else if (atm->state == DEVNULL)
	{
		GPrint_handler = gprint_to_devnull;
		dst = atm->files->redirected;
	} else
		dst = atm->files->original;

	/*
	 * Don't change $stdout if it's set by the application.
	 * This case don't close $atm->redirected either because
	 * we may share it with the other $atm.
	 * It would be nice to fclose(probe) but glibc doesn't like that
	 * if we're in its write() callback.
	 */
	if (stdout == atm->files->probe)
		stdout = redirected(STDOUT_FILENO)
			? atm->files->original : dst;

	/* Likewise, take care not to override application-set handlers
	 * unless we're configured to screw it. */
	prev = g_set_print_handler(GPrint_handler);
	if (!atm->screwit && prev != probe_gprint)
		g_set_print_handler(GPrint_handler = prev);
} /* setup_stdout */

/* Figure out the final $stderr, $GPrintErr_handler and $GLog_handler. */
static void setup_stderr(struct artem_state_st *atm)
{
	FILE *dst;
	GLogFunc prev_log;
	GPrintFunc prev_print;

	if ((atm->state == REDIR || atm->state == DEVNULL)
			&& redirected(STDERR_FILENO))
		atm->files->redirected = atm->files->original;

	if (atm->state == REDIR)
	{
		GPrintErr_handler = gprint_to_stderr;
		GLog_handler = glog_to_stderr;
		dst = atm->files->redirected;
	} else if (atm->state == DEVNULL)
	{
		GPrintErr_handler = gprint_to_devnull;
		GLog_handler = glog_to_devnull;
		dst = atm->files->redirected;
	} else
		dst = atm->files->original;

	if (stderr == atm->files->probe)
		stderr = dst;

	prev_print = g_set_printerr_handler(GPrintErr_handler);
	if (!atm->screwit && prev_print != probe_gprinterr)
		g_set_printerr_handler(GPrintErr_handler = prev_print);

	prev_log = real_g_log_set_default_handler(GLog_handler, NULL);
	if (!atm->screwit && prev_log != probe_glog)
		real_g_log_set_default_handler(
			GLog_handler = prev_log, GLog_handler_userdata);
} /* setup_stderr */

/* Tell about an error we encountered while trying to open a redirection
 * destination file.  Only used after the final $GLog_handler has been
 * determined. */
static void setup_error(struct artem_state_st *err, char const *msg, int error)
{
	char buf[128];

	/* Use the handler directly because we may have come from
	 * probe_glog(). */
	snprintf(buf, sizeof(buf), "%s: %s", msg, strerror(error));
	GLog_handler("artem", G_LOG_LEVEL_WARNING, buf,
		GLog_handler_userdata);
} /* setup_error */

/* Set up all of the final output handlers. */
static void setup(struct artem_state_st *out, struct artem_state_st *err)
{
	char *p;
	int serrno;

	DEBUG("setup()");
	serrno = errno;
	if ((p = strrchr(program_invocation_short_name, '/')) != NULL)
		/* glibc may have got it wrong */
		program_invocation_short_name = p + 1;

	/* Do our mission. */
	config(out, err);
	setup_stdout(out);
	setup_stderr(err);

	/* Now that $GLog_handler is finalized we can print errors. */
	if (out->state == ERROR)
		setup_error(err, "Could not redirect stdout", out->error);
	if (err->state == ERROR)
		setup_error(err, "Could not redirect stderr", err->error);
	DEBUG("setup() finish");

	errno = serrno;
} /* setup */
/* Setup }}} */

/* Probes {{{
 *
 * These are dummy output handlers used to wake up libartem to set up
 * the final output handlers.  When done they print their original message.
 */
/* For $stdout and $stderr */
static ssize_t probe_stdio(void *st, char const *buf, size_t sbuf)
{
	struct artem_state_st out, err;

	DEBUG("probe_stdio()");
	setup(&out, &err);
	DEBUG("writeback");
	fwrite(buf, sbuf, 1, st == out.files->original ? stdout : stderr);
	DEBUG("probe_stdio() finish");

	return sbuf;
} /* probe_stdio */

/* For g_print() */
static void probe_gprint(char const *str)
{
	struct artem_state_st out, err;

	DEBUG("probe_gprint()");
	setup(&out, &err);
	GPrint_handler(str);
	DEBUG("probe_gprint() finish");
} /* probe_gprint */

/* For g_printerr() */
static void probe_gprinterr(char const *str)
{
	struct artem_state_st out, err;

	DEBUG("probe_gprinterr()");
	setup(&out, &err);
	GPrintErr_handler(str);
	DEBUG("probe_gprinterr() finish");
} /* probe_gprinterr */

/* For g_log() */
static void probe_glog(gchar const *domain, GLogLevelFlags level,
	gchar const *message, gpointer unused)
{
	struct artem_state_st out, err;

	/* It is important not to g_log() here because glib wouldn't like
	 * the recursion. */
	DEBUG("probe_glog()");
	setup(&out, &err);
	GLog_handler(domain, level, message, unused);
	DEBUG("probe_glog() finish");
} /* probe_glog */
/* Probes }}} */

/* Init {{{ */
#ifdef CONFIG_PEDANTIC
/* Proxy function to track $udata, so we can precisely restore
 * the handler if it's set by the user. */
GLogFunc g_log_set_default_handler(GLogFunc fun, gpointer udata)
{
	/* Needn't save $fun, we'll know what to restore.
	 * Don't let it change if we should screwit(). */
	GLog_handler_userdata = udata;
	fun = real_g_log_set_default_handler(fun, udata);
	if (screwit(-1))
		real_g_log_set_default_handler(fun, NULL);
	return fun;
}
#endif

/*
 * Initialize delayed setup() by installing probe functions.
 * Try to make sure we can restore the pristine settings if
 * the user doesn't want us to override them after all or we
 * fail to do so for some reason.
 */
void __attribute__((constructor)) init(void)
{
	static cookie_io_functions_t ops = { .write = probe_stdio };

	DEBUG("init()");

#ifdef CONFIG_PEDANTIC
	/* Find $real_g_log_set_default_handler. */
	dlerror();
	real_g_log_set_default_handler = dlsym(RTLD_NEXT,
		"g_log_set_default_handler");
	if (!real_g_log_set_default_handler && dlerror())
	{	/* The impossible happened, let's do no more damage. */
		fprintf(stderr, "i've just screwed up your logging :)\n");
		return;
	}
#endif /* CONFIG_PEDANTIC */

	Stdout.original		= stdout;
	Stderr.original		= stderr;

	Stdout.probe = stdout	= fopencookie(Stdout.original, "w", ops);
	Stderr.probe = stderr	= fopencookie(Stderr.original, "w", ops);

	/* Be unbuffered so probe_stdio() will get the whole first output. */
	setvbuf(Stdout.probe, NULL, _IONBF, 0);
	setvbuf(Stderr.probe, NULL, _IONBF, 0);

	GPrint_handler		= g_set_print_handler(probe_gprint);
	GPrintErr_handler	= g_set_printerr_handler(probe_gprinterr);
	GLog_handler		= real_g_log_set_default_handler(
					probe_glog, NULL);

	/* glib won't give us the original functions */
	if (!GPrint_handler)
		GPrint_handler    = gprint_to_original_stdout;
	if (!GPrintErr_handler)
		GPrintErr_handler = gprint_to_original_stderr;
} /* init */
/* Init }}} */

/* vim: set foldmethod=marker: */
/* End of libartem.c */
