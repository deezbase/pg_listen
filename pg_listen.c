#include <errno.h>
#include <libpq-fe.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void		listen_forever(PGconn *, const char *, const char *, char **);
int			reset_if_necessary(PGconn *);
void		clean_and_die(PGconn *);
void		begin_listen(PGconn *, const char *);
int			print_log(const char *, const char *, ...);
int			exec_pipe(const char *cmd, char **cmd_argv, const char *input);

#define		BUFSZ 512

int
main(int argc, char **argv)
{
	PGconn	   *conn;
	char	   *chan;

	if (argc < 3)
	{
		fprintf(stderr,
				"USAGE: %s db-url channel [/path/to/program] [args]\n",
				argv[0]);
		return EXIT_FAILURE;
	}

	/* if no command given, print payload with line buffering */
	if (argc == 3)
		setvbuf(stdout, NULL, _IOLBF, 0);

	conn = PQconnectdb(argv[1]);
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fputs(PQerrorMessage(conn), stderr);
		clean_and_die(conn);
	}

	chan = PQescapeIdentifier(conn, argv[2], BUFSZ);
	if (chan == NULL)
	{
		fputs(PQerrorMessage(conn), stderr);
		clean_and_die(conn);
	}

	/* safe since argv[argc] == NULL by C99 5.1.2.2.1 */
	listen_forever(conn, chan, argv[3], argv+3);

	/* should never get here */
	PQfreemem(chan);
	PQfinish(conn);
	return 0;
}

int
exec_pipe(const char *cmd, char **cmd_argv, const char *input)
{
	int			pipefds[2];

	/* we'll send "input" through pipe to stdin */
	if (errno = 0, pipe(pipefds) < 0)
	{
		perror("pipe()");
		return 0;
	}

	switch (errno = 0, fork())
	{
		case -1:
			perror("fork()");
			close(pipefds[0]);
			close(pipefds[1]);
			return 0;
		case 0: /* Child - reads from pipe */
			/* Write end is unused */
			close(pipefds[1]);
			/* read from pipe as stdin */
			if (errno = 0, dup2(pipefds[0], STDIN_FILENO) < 0)
			{
				perror("Unable to assign stdin to pipe");
				close(pipefds[0]);
				exit(EXIT_FAILURE);
			}
			if (errno = 0, execv(cmd, cmd_argv) < 0)
			{
				print_log("CRITICAL", "Can't run %s: %s",
						cmd, strerror(errno));
				close(pipefds[0]);
				exit(EXIT_FAILURE);
			}
			/* should not get here */
			break;
		default: /* Parent - writes to pipe */
			close(pipefds[0]); /* Read end is unused */
			write(pipefds[1], input, strlen(input));
			close(pipefds[1]);
			break;
	}
	return 1;
}

void
listen_forever(PGconn *conn, const char *chan, const char *cmd, char **cmd_argv)
{
	int			sock;
	PGnotify   *notify;
	struct pollfd pfd[1];

	begin_listen(conn, chan);

	while (1)
	{
		if (reset_if_necessary(conn))
			begin_listen(conn, chan);

		sock = PQsocket(conn);
		if (sock < 0)
		{
			fprintf(stderr, "Failed to get libpq socket: %s\n",
			        PQerrorMessage(conn));
			clean_and_die(conn);
		}

		pfd[0].fd = sock;
		pfd[0].events = POLLIN;
		if (errno = 0, poll(pfd, 1, -1) < 0)
		{
			perror("poll()");
			clean_and_die(conn);
		}

		PQconsumeInput(conn);
		while ((notify = PQnotifies(conn)) != NULL)
		{
			if (!cmd)
				fputs(notify->extra, stdout);
			else if (!exec_pipe(cmd, cmd_argv, notify->extra))
			{
				PQfreemem(notify);
				clean_and_die(conn);
			}

			PQfreemem(notify);
		}
	}
}

int
reset_if_necessary(PGconn *conn)
{
	unsigned int seconds = 0;

	if (PQstatus(conn) == CONNECTION_OK)
		return 0;

	do
	{
		if (seconds == 0)
			seconds = 1;
		else
		{
			print_log("ERROR", "Connection failed.\nSleeping %u seconds.", seconds);
			sleep(seconds);
			seconds *= 2;
		}
		print_log("INFO", "Reconnecting to database...");
		PQreset(conn);
	} while (PQstatus(conn) != CONNECTION_OK);

	print_log("INFO", "Connected.");
	return 1;
}

void
begin_listen(PGconn *conn, const char *chan)
{
	PGresult   *res;
	char		sql[7 + BUFSZ + 1];

	print_log("INFO", "Listening for channel %s", chan);

	snprintf(sql, 7 + BUFSZ + 1, "LISTEN %s", chan);
	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		print_log("CRITICAL", "LISTEN command failed: %s", PQerrorMessage(conn));
		PQclear(res);
		clean_and_die(conn);
	}
	PQclear(res);
}

void
clean_and_die(PGconn *conn)
{
	PQfinish(conn);
	exit(EXIT_FAILURE);
}

int
print_log(const char *sev, const char *fmt, ...)
{
	va_list	ap;
	time_t	now = time(NULL);
	char	timestamp[128];
	int		res;

	strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%S", gmtime(&now));
	res = fprintf(stderr, "%s - pg_listen - %s - ", timestamp, sev);

	va_start(ap, fmt);
	res += vfprintf(stderr, fmt, ap);
	va_end(ap);

	return res + fprintf(stderr, "\n");
}
