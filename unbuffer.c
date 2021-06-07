/*
 * unbuffer.c -- execute a program and make its output unbuffered
 *
 * You know the situation: some program print diagnostic information
 * and you want to save it to a file.  As you know stdout becomes
 * buffered if it's not connected to a terminal, and interrupting
 * the program doesn't flush it automatically either.
 *
 * `unbuffer' starts the given program, makes it believe it talks
 * to the terminal (making its stdout unbuffered) and then forward
 * its output to stdout unbuffered.  If you redirect `unbuffer's
 * stdout, you'll get instant unbuffered output.  stdin is also
 * forwarded to the child program.
 *
 * Usage: ./unbuffer <prog> [args]...
 */

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>

/* Program code */
int main(int argc, char *argv[])
{
	int fd;
	struct termios term;

	if (!argv[1])
		return 0;

	/* Start the child program.  $fd is like a terminal:
	 * we can write to it, which the child will see as input
	 * and we'll see the child's output as input on $fd. */
	if (!forkpty(&fd, NULL, NULL, NULL))
	{
		execvp(argv[1], &argv[1]);
		fprintf(stderr, "%s: %m", argv[1]);
		return 1;
	}

	/* Set stdin and $fd non-blocking.  Set stdin raw,
	 * so we'll be able to catch EOF. */
	fcntl(fd, F_SETFL, O_NONBLOCK);
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	tcgetattr(STDIN_FILENO, &term);
	term.c_lflag &= ~ICANON;
	tcsetattr(STDIN_FILENO, TCSANOW, &term);

	/* The main loop. */
	for (;;)
	{
		fd_set rfd;
		int lbuf;
		char buf[128];

		/* Wait until there's something to forward. */
		FD_ZERO(&rfd);
		FD_SET(fd, &rfd);
		FD_SET(STDIN_FILENO, &rfd);
		if (select(fd+1, &rfd, NULL, NULL, NULL) <= 0)
			break;

		/* Forward the child's output to stdout. */
		if (FD_ISSET(fd, &rfd))
			while ((lbuf = read(fd, buf, sizeof(buf))) > 0)
				write(STDOUT_FILENO, buf, lbuf);

		/* Forward stdin to the child. */
		if (FD_ISSET(STDIN_FILENO, &rfd))
			while ((lbuf = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
			{
				if (buf[0] == '')
				{	/* EOF */
					close(fd);
					return 0;
				}
				write(fd, buf, lbuf);
			}
	} /* for */

	return 0;
} /* main */

/* End of unbuffer.c */
