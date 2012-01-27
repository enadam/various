/*
 * dspsrv.c -- start a TCP/IP server and dump all traffic to /dev/dsp
 *
 * This program allows you to set up a quick'n'dirty sound server.
 * It prepares the sound cards output device (/dev/dsp) for 44kHZ, 16 bit,
 * stereo input, waits for a TCP/IP connection and sends all received data
 * to the sound card.  You can send anything /dev/dsp understands; .wav is
 * a reasonable and working choice.  After the connection ends the program
 * awaits for a new one until it is terminated by a signal (eg. <CTRL-C>).
 *
 * To establish a network server, this program uses nc(1), which should be
 * installed in /bin.  On the other side you can use nc(1) too, eg. like
 * ``mpg321 -s buzievagy.mp3 | nc <host> <port>''.
 * 
 * Invocation:
 *   dspsrv [<port>]		Sound server mode.  <port> tells which TCP
 *				port to listen on.  Leaving unspecified
 *				defaults to DFLT_PORT.
 *   dspsrv 0			Don't listen at all, but after setting up
 *				/dev/dsp for HQ audio, start relaying stdin
 *				there.
 *   dspsrv <cmd>		Likewise, but execute <cmd> having its stdout
 *				redirected to /dev/dsp.
 *
 * If the caller has right to write /dev/dsp and <port> is less than
 * 1024 this program doesn't need extra privileges.
 */

/* Include files */
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/wait.h>

/* Standard definitions */
/* Default port to listen on if not specified on the command line. */
#define DFLT_PORT			"96984"

/* Global variable declarations. */
/* The array of environment variables, needed by execve(). */
extern char **environ;

/* Program code */
/* The main function */
int main(int argc, char *argv[])
{
	int fd;
	unsigned val;
	static char *args[] = { "nc", "-l", "-p", DFLT_PORT, NULL };

	/* fd <- /dev/dsp
	 * stdout <- fd, so nc(1) will output there. */
	if ((fd = open("/dev/dsp", O_WRONLY)) < 0)
	{
		perror("/dev/dsp");
		return 1;
	} else if (fd != STDOUT_FILENO)
	{
		dup2(fd, STDOUT_FILENO);
		close(fd);
	}

	/* Set up /dev/dsp. */
        /* Set stereo mode. */
        val = 1;
        if (ioctl(STDOUT_FILENO, SNDCTL_DSP_STEREO, &val) == -1)
	{
                perror("STEREO");
		return 1;
	}

        /* Set 16 bit mod. */
        val = 16;
        if (ioctl(STDOUT_FILENO, SNDCTL_DSP_SAMPLESIZE, &val) == -1)
	{
		perror("SAMPLESIZE");
		return 1;
	}

        /* Set 44kHz playback frequency. */
	val = 44100;
        if (ioctl(STDOUT_FILENO, SNDCTL_DSP_SPEED, &val) == -1)
	{
		perror("SPEED");
		return 1;
	}

	/* Let's stop for a moment, considering $argv[]. */
	if (argv[1])
	{
		if (atoi(argv[1]) != 0)
		{
			/* Tell nc(1) to recv() on a differenct port. */
			args[3] = argv[1];
		} else if (!isdigit(argv[1][0]))
		{
			/* Execute $argv[1] with stdout bound to /dev/dsp. */
			execvp(argv[1], &argv[1]);
			perror(argv[1]);
			exit(1);
		} else
		{
			static char *cat[] = { "cat", NULL };

			/* Execute `cat' and dump stdin to /dev/dsp. */
			execvp(cat[0], cat);
			perror(cat[0]);
			exit(1);
		}
	}

	/* We're going to run nc(1). */
	/* stdin <- /dev/null, so we can go to the background. */
	if ((fd = open("/dev/null", O_RDONLY)) < 0)
	{
		perror("/dev/null");
		return 1;
	} else if (dup2(fd, STDIN_FILENO) < 0)
	{
		perror("Couldn't redirect stdin to /dev/null");
		return 1;
	}

	/* Start and restart nc(1) on and on. */
	for (;;)
	{
		switch (fork())
		{
		case -1:
			/* Don't try again, we would loop. */
			perror("fork");
			exit(1);
		case 0:
			/* Try out different locations before giving up. */
			execve("/bin/nc",     args, environ);
			execve("/usr/bin/nc", args, environ);
			perror("nc");
			exit(1);
		default:
			/* Wait until nc(1) finishes. */
			wait(NULL);
		}
	} /* for */
} /* main */

/* End of dspsrv.c */
