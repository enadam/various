/*
 * tgconscp.c -- scponlyc replacement
 *
 * This implementation chroot()s into the home of the login user,
 * drops *all* privileges and chdir()s into the home in the chroot.
 * Works both with scp(1) and sftp(1).  All messages are syslog()ed
 * as LOG_DAEMON.
 *
 * In order to chroot() this program must be installed as suid root.
 */

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>

#include <string.h>
#include <stdio.h>
#include <syslog.h>
#include <pwd.h>

/* Standard definitions */
/*
 * SCP_PATH, SFTP_PATH:	path to the appropriate "backend" binaries
 *			in the chroot
 * SSH_MAXARGS:		maximum number of arguments to be passed
 *			to the backends
 */
#define SCP_PATH	"/usr/bin/scp"
#define SFTP_PATH	"/usr/lib/openssh/sftp-server"
#define SSH_MAXARGS	8

/* Program code */
/* Private functions */
/* syslog()s the arguments with LOG_ERR and exits with an error.
 * Use "%m" to log the textual representation of `errno'. */
static void __attribute__((noreturn)) die(char const *fmt, ...)
{
	va_list syslog_args;

	va_start(syslog_args, fmt);
	vsyslog(LOG_ERR, fmt, syslog_args);
	va_end(syslog_args);
	exit(1);
} /* die */

/*
 * Returns the start of the next token (sequence of non-space characters)
 * from `*strp', or NULL if no token found.  Any leading whitespace is
 * skipped.  The returned token is null-terminated, modifying `*strp',
 * except if `to_end' is true.  `*strp' is updated to point to the next
 * character after the termination, so you can call nexttoken() again
 * with the same argument to get the following token.
 */
static char *nexttoken(char **strp, int to_end)
{
	char *start;

	/* Skip leading whitespace. */
	for (;;)
	{
		if (**strp == '\0')
			/* End of string, found no token. */
			return NULL;
		else if (!isspace(**strp))
			/* Non-whitespace, start of token found. */
			break;
	} /* for */

	/* Preserve the original termination of `*strp' if `to_end'. */
	start = *strp;
	if (to_end)
		return start;

	/* Find the end of the token and terminate it. */
	for (;;)
	{
		/* The first character of `start' is always !isspace(). */
		(*strp)++;
		if (isspace(**strp))
		{
			*(*strp)++ = '\0';
			break;
		} else if (**strp == '\0')
			/* End of string reached. */
			break;
	} /* for */

	return start;
}

/* The main function */
int main(int argc, char *argv[], char *envp[])
{
	struct passwd const *pwd;
	char *tok, *scpargs[SSH_MAXARGS + 1];

	openlog("tgconscp", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	/* sshd(8) should have started us into the home of the login user.
	 * chroot() there, and drop all remaining privileges */
	if (chroot(".") < 0)
		die("chroot: %m");
	if (setuid(getuid()) < 0)
		die("setuid(%u): %m", getuid());
	if (getuid() == 0 || getuid() != geteuid())
		die("uid still privileged");
	if (getgid() == 0 || getgid() != getegid())
		die("gid still privileged");

	/* Query the $HOME of the user in the chroot and chdir() there.
	 * This implies that we have a /etc/passwd in the chroot. */
	if (!(pwd = getpwuid(getuid())))
		die("getpwuid(%u): %m", getuid());
	if (chdir(pwd->pw_dir) < 0)
		die("chdir(%s): %m", pwd->pw_dir);

	/*
	 * Start compiling the execve() argument list.  sshd(8) have
	 * passed us a "-c" "tattara", the latter being the command
	 * line we are expected to execute (like shells).  execve()
	 * expects arrays rather than command lines we'll need to
	 * split it.
	 */
	/* The first "-c" argument is mandatory. */
	if (argv[1] == NULL)
		die("argv[1]: too few arguments");
	if (strcmp(argv[1], "-c"))
		die("argv[1]: `%s', expected `-c'", argv[1]);

	/* Process the command line following "-c".  Extract the backend
	 * binary name first and continue parsing the string accordingly. */
	if (argv[2] == NULL)
		die("argv[2]: too few arguments");
	if (!(tok = nexttoken(&argv[2], 0)))
		die("argv[2]: program to execute expected");
	if (!strcmp(tok, "scp") || !strcmp(tok, SCP_PATH))
	{
		/*
		 * We're invoked to scp(1).  The command line looks like:
		 * "{scp|SCP_PATH} -{t|f} <path>" this case, "-t" ("to")
		 * for uploading, "-f" ("from") for downloading.
		 * <path> lasts till the end of the command line, thus
		 * it may contain spaces; NOTE, however, that it cannot
		 * start with whitespace.
		 */
		scpargs[0] = SCP_PATH;
		if (!(scpargs[1] = nexttoken(&argv[2], 0)))
			die("argv[2]: `%s', expected `scp -[tf] <fname>'",
				argv[2]);
		if (strcmp(scpargs[1], "-f") && strcmp(scpargs[1], "-t"))
			die("argv[2]: `%s', expected `scp -[tf] <fname>'",
				scpargs[1]);
		if (!(scpargs[2] = nexttoken(&argv[2], 1)))
			die("argv[2]: `%s', expected `scp -[tf] <fname>'",
				argv[2]);

		/* Terminate the argument list. */
		scpargs[3] = NULL;
	} else if (!strcmp(tok, SFTP_PATH))
	{
		unsigned i;

		/* We're invoked for sftp(1).  Relay all command line
		 * options up to SSH_MAXARGS; complain if limit reached. */
		i = 0;
		scpargs[i++] = SFTP_PATH;
		while ((tok = nexttoken(&argv[2], 0)) != NULL)
		{
			/* `scpargs' is one element longer than SSH_MAXARGS
			 * to allow for termination. */
			if (i >= SSH_MAXARGS)
				die("sftp: too many arguments");
			scpargs[i++] = tok;
		}
		scpargs[i] = NULL;
	} else	/* WTF do you want? */
		die("argv[2]: `%s', `%s' or `%s' expected",
			"scp", SCP_PATH, SFTP_PATH);

	/* Expect no arguments after the command line. */
	if (argv[3] != NULL)
		die("argv[3]: too many arguments");

	/* Go!  If execve() returns something when wrong. */
	execve(scpargs[0], scpargs, envp);
	die("exec(%s): %m", scpargs[0]);
} /* main */

/* End of tgconscp.c */
