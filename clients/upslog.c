/* upslog - log ups values to a file for later collection and analysis

   Copyright (C) 1998  Russell Kroll <rkroll@exploits.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* Basic theory of operation:
 *
 * First we go through and parse as much of the status format string as
 * possible.  We used to do this parsing run every time, but that's a
 * waste of CPU since it can't change during the program's run.
 *
 * This version does the parsing pass once, and creates a linked list of
 * pointers to the functions that do the work and the arg they get.
 *
 * That means the main loop just has to run the linked list and call
 * anything it finds in there.  Everything happens from there, and we
 * don't have to pointlessly reparse the string every time around.
 */

#include "common.h"
#include "nut_platform.h"
#include "upsclient.h"

#include "config.h"
#include "timehead.h"
#include "nut_stdint.h"
#include "upslog.h"
#include "str.h"

#ifdef WIN32
#include "wincompat.h"
#endif

	static	int	reopen_flag = 0, exit_flag = 0;
	static	size_t	max_loops = 0;
	static	char	*upsname;
	static	UPSCONN_t	*ups;

	static	char *logfn, *monhost;
#ifndef WIN32
	static	sigset_t	nut_upslog_sigmask;
#endif
	static	char	logbuffer[LARGEBUF], *logformat;

	static	flist_t	*fhead = NULL;
	struct 	logtarget_t {
		char	*logfn;
		FILE	*logfile;
		struct 	logtarget_t	*next;
	};
	/* FIXME: To be valgrind-clean, free these at exit */
	static	struct	logtarget_t *logfile_anchor = NULL;

	struct 	monhost_ups {
		char	*monhost;
		char	*upsname;
		char	*hostname;
		uint16_t	port;
		UPSCONN_t	*ups;
		struct 	logtarget_t	*logtarget;
		struct	monhost_ups	*next;
	};
	/* FIXME: To be valgrind-clean, free these at exit */
	static	struct	monhost_ups *monhost_ups_anchor = NULL;
	static	struct	monhost_ups *monhost_ups_current = NULL;
	static	struct	monhost_ups *monhost_ups_prev = NULL;


#define DEFAULT_LOGFORMAT "%TIME @Y@m@d @H@M@S% %VAR battery.charge% " \
		"%VAR input.voltage% %VAR ups.load% [%VAR ups.status%] " \
		"%VAR ups.temperature% %VAR input.frequency%"

static struct logtarget_t *get_logfile(const char *logfn_arg)
{
	struct	logtarget_t	*p = NULL;

	if (!logfn_arg || !(*logfn_arg) || !logfile_anchor)
		return p;

	for (p = logfile_anchor; p != NULL; p = p->next) {
		if (!strcmp(logfn_arg, p->logfn))
			return p;
	}

	/* NULL by now */
	return p;
}

static struct logtarget_t *add_logfile(const char *logfn_arg)
{
	struct	logtarget_t	*p = get_logfile(logfn_arg);

	if (p)
		return p;

	/* Ignore bogus additions; FIXME: Error out? */
	if (!logfn_arg || !(*logfn_arg))
		return p;

	p = xcalloc(1, sizeof(struct monhost_ups));
	p->logfn = xstrdup(logfn_arg);
	p->logfile = NULL;

	/* Inject into the chain, head is as good a place as any */
	p->next = logfile_anchor;
	logfile_anchor = p;

	return p;
}

static void reopen_log(void)
{
	struct	logtarget_t	*p;

	for (p = logfile_anchor;
	     p != NULL;
	     p = p->next
	) {
		if (p->logfile == stdout) {
			upslogx(LOG_INFO, "logging to stdout");
			continue;
		}

		if ((p->logfile = freopen(
		    p->logfn, "a",
		    p->logfile)) == NULL
		) {
			fatal_with_errno(EXIT_FAILURE,
				"could not reopen logfile %s", p->logfn);
		}
	}
}

#ifndef WIN32
static void set_reopen_flag(int sig)
{
	reopen_flag = sig;
}

static void set_exit_flag(int sig)
{
	exit_flag = sig;
}

static void set_print_now_flag(int sig)
{
	NUT_UNUSED_VARIABLE(sig);

	/* no need to do anything, the signal will cause sleep to be interrupted */
}
#endif

/* handlers: reload on HUP, exit on INT/QUIT/TERM */
static void setup_signals(void)
{
#ifndef WIN32
	struct	sigaction	sa;

	sigemptyset(&nut_upslog_sigmask);
	sigaddset(&nut_upslog_sigmask, SIGHUP);
	sa.sa_mask = nut_upslog_sigmask;
	sa.sa_handler = set_reopen_flag;
	sa.sa_flags = 0;
	if (sigaction(SIGHUP, &sa, NULL) < 0)
		fatal_with_errno(EXIT_FAILURE, "Can't install SIGHUP handler");

	sa.sa_handler = set_exit_flag;
	if (sigaction(SIGINT, &sa, NULL) < 0)
		fatal_with_errno(EXIT_FAILURE, "Can't install SIGINT handler");
	if (sigaction(SIGQUIT, &sa, NULL) < 0)
		fatal_with_errno(EXIT_FAILURE, "Can't install SIGQUIT handler");
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		fatal_with_errno(EXIT_FAILURE, "Can't install SIGTERM handler");

	sa.sa_handler = set_print_now_flag;
	if (sigaction(SIGUSR1, &sa, NULL) < 0)
		fatal_with_errno(EXIT_FAILURE, "Can't install SIGUSR1 handler");
#endif
}

static void help(const char *prog)
	__attribute__((noreturn));

static void help(const char *prog)
{
	print_banner_once(prog, 2);
	printf("NUT read-only client program - UPS status logger.\n");

	printf("\nusage: %s [OPTIONS]\n", prog);
	printf("\n");

	printf("  -f <format>	- Log format.  See below for details.\n");
	printf("		- Use -f \"<format>\" so your shell doesn't break it up.\n");
	printf("  -i <interval>	- Time between updates, in seconds\n");
	printf("  -d <count>	- Exit after specified amount of updates\n");
	printf("  -l <logfile>	- Log file name, or - for stdout (foreground by default)\n");
	printf("  -D		- raise debugging level (and stay foreground by default)\n");
	printf("  -F		- stay foregrounded even if logging into a file\n");
	printf("  -B		- stay backgrounded even if logging to stdout or debugging\n");
	printf("  -p <pidbase>  - Base name for PID file (defaults to \"%s\")\n", prog);
	printf("                - NOTE: PID file is written regardless of fore/back-grounding\n");
	printf("  -s <ups>	- Monitor UPS <ups> - <upsname>@<host>[:<port>]\n");
	printf("        	- Example: -s myups@server\n");
	printf("  -m <tuple>	- Monitor UPS <ups,logfile>\n");
	printf("		- Example: -m myups@server,/var/log/myups.log\n");
	printf("		- NOTE: You can use '-' as logfile for stdout\n");
	printf("		  and it would not imply foregrounding\n");
	printf("		- Unlike one '-s ups -l file' spec, you can specify many tuples\n");
	printf("  -u <user>	- Switch to <user> if started as root\n");
	printf("  -V		- Display the version of this software\n");
	printf("  -h		- Display this help text\n");

	printf("\n");
	printf("Some valid format string escapes:\n");
	printf("\t%%%% insert a single %%\n");
	printf("\t%%TIME format%% insert the time with strftime formatting\n");
	printf("\t%%HOST%% insert the local hostname\n");
	printf("\t%%UPSHOST%% insert the host of the ups being monitored\n");
	printf("\t%%PID%% insert the pid of upslog\n");
	printf("\t%%VAR varname%% insert the value of ups variable varname\n\n");
	printf("format string defaults to:\n");
	printf("%s\n", DEFAULT_LOGFORMAT);

	nut_report_config_flags();

	printf("\n%s", suggest_doc_links(prog, NULL));

	exit(EXIT_SUCCESS);
}

/* print current host name */
static void do_host(const char *arg)
{
	int	ret;
	char	hn[LARGEBUF];
	NUT_UNUSED_VARIABLE(arg);

	ret = gethostname(hn, sizeof(hn));

	if (ret != 0) {
		upslog_with_errno(LOG_ERR, "gethostname failed");
		return;
	}

	snprintfcat(logbuffer, sizeof(logbuffer), "%s", hn);
}

static void do_upshost(const char *arg)
{
	NUT_UNUSED_VARIABLE(arg);

	snprintfcat(logbuffer, sizeof(logbuffer), "%s", monhost);
}

static void do_pid(const char *arg)
{
	NUT_UNUSED_VARIABLE(arg);

	snprintfcat(logbuffer, sizeof(logbuffer), "%ld", (long)getpid());
}

static void do_time(const char *arg)
{
	unsigned int	i;
	char	timebuf[SMALLBUF], *format;
	time_t	tod;
	struct tm tmbuf;

	format = xstrdup(arg);

	/* @s are used on the command line since % is taken */
	for (i = 0; i < strlen(format); i++)
		if (format[i] == '@')
			format[i] = '%';

	time(&tod);
	strftime(timebuf, sizeof(timebuf), format, localtime_r(&tod, &tmbuf));

	snprintfcat(logbuffer, sizeof(logbuffer), "%s", timebuf);

	free(format);
}

static void getvar(const char *var)
{
	int	ret;
	size_t	numq, numa;
	const	char	*query[4];
	char	**answer;

	query[0] = "VAR";
	query[1] = upsname;
	query[2] = var;
	numq = 3;

	ret = upscli_get(ups, numq, query, &numa, &answer);

	if ((ret < 0) || (numa < numq)) {
		snprintfcat(logbuffer, sizeof(logbuffer), "NA");
		return;
	}

	snprintfcat(logbuffer, sizeof(logbuffer), "%s", answer[3]);
}

static void do_var(const char *arg)
{
	if ((!arg) || (strlen(arg) < 1)) {
		snprintfcat(logbuffer, sizeof(logbuffer), "INVALID");
		return;
	}

	/* old variable names are no longer supported */
	if (!strchr(arg, '.')) {
		snprintfcat(logbuffer, sizeof(logbuffer), "INVALID");
		return;
	}

	/* a UPS name is now required */
	if (!upsname) {
		snprintfcat(logbuffer, sizeof(logbuffer), "INVALID");
		return;
	}

	getvar(arg);
}

static void do_etime(const char *arg)
{
	time_t	tod;
	NUT_UNUSED_VARIABLE(arg);

	time(&tod);
	snprintfcat(logbuffer, sizeof(logbuffer), "%ld", (unsigned long) tod);
}

static void print_literal(const char *arg)
{
	snprintfcat(logbuffer, sizeof(logbuffer), "%s", arg);
}

/* register another parsing function to be called later */
static void add_call(void (*fptr)(const char *arg), const char *arg)
{
	flist_t	*tmp, *last;

	tmp = last = fhead;

	while (tmp) {
		last = tmp;
		tmp = tmp->next;
	}

	tmp = xmalloc(sizeof(flist_t));

	tmp->fptr = fptr;

	if (arg)
		tmp->arg = xstrdup(arg);
	else
		tmp->arg = NULL;

	tmp->next = NULL;

	if (last)
		last->next = tmp;
	else
		fhead = tmp;
}

/* turn the format string into a list of function calls with args */
static void compile_format(void)
{
	size_t	i;
	int	j, found;
	size_t	ofs;
	char	*cmd, *arg, *ptr;

	for (i = 0; i < strlen(logformat); i++) {

		/* if not a % sequence, append character and start over */
		if (logformat[i] != '%') {
			char	buf[4];

			/* we have to stuff it into a string first */
			snprintf(buf, sizeof(buf), "%c", logformat[i]);
			add_call(print_literal, buf);

			continue;
		}

		/* Safe to peek into i+1, at worst would be '\0' */
		/* if a %%, append % and start over */
		if (logformat[i+1] == '%') {
			add_call(print_literal, "%");

			/* make sure we don't parse the second % next time */
			i++;
			continue;
		}

		/* if %t, append a TAB character and start over */
		if (logformat[i+1] == 't') {
			add_call(print_literal, "\t");

			/* make sure we don't parse the 't' next time */
			i++;
			continue;
		}

		/* it must start with a % now - %<cmd>[ <arg>]%*/

		cmd = xstrdup(&logformat[i+1]);
		ptr = strchr(cmd, '%');

		/* no trailing % = broken */
		if (!ptr) {
			add_call(print_literal, "INVALID");
			free(cmd);
			continue;
		}

		*ptr = '\0';

		/* remember length (plus first %) so we can skip over it */
		ofs = strlen(cmd) + 1;

		/* jump out to argument (if any) */
		arg = strchr(cmd, ' ');
		if (arg)
			*arg++ = '\0';

		found = 0;

		/* see if we know how to handle this command */

		for (j = 0; logcmds[j].name != NULL; j++) {
			if (strncasecmp(cmd, logcmds[j].name,
				strlen(logcmds[j].name)) == 0) {

				add_call(logcmds[j].func, arg);
				found = 1;
				break;
			}
		}

		free(cmd);

		if (!found)
			add_call(print_literal, "INVALID");

		/* now do the skip ahead saved from before */
		i += ofs;

	} /* for (i = 0; i < strlen(logformat); i++) */
}

/* go through the list of functions and call them in order */
static void run_flist(struct monhost_ups *monhost_ups_print)
{
	flist_t	*tmp;

	tmp = fhead;

	memset(logbuffer, 0, sizeof(logbuffer));

	while (tmp) {
		tmp->fptr(tmp->arg);

		tmp = tmp->next;
	}

	fprintf(monhost_ups_print->logtarget->logfile, "%s\n", logbuffer);
	fflush(monhost_ups_print->logtarget->logfile);
}

	/* -s <monhost>
	 * -l <log file>
	 * -i <interval>
	 * -f <format>
	 * -u <username>
	 */

int main(int argc, char **argv)
{
	int	interval = 30, i, foreground = -1;
	size_t	monhost_len = 0, loop_count = 0;
	const char	*prog = xbasename(argv[0]);
	time_t	now, nextpoll = 0;
	const char	*user = NULL;
	struct passwd	*new_uid = NULL;
	const char	*pidfilebase = prog;

	logformat = DEFAULT_LOGFORMAT;
	user = RUN_AS_USER;

	print_banner_once(prog, 0);

	while ((i = getopt(argc, argv, "+hDs:l:i:d:f:u:Vp:FBm:")) != -1) {
		switch(i) {
			case 'h':
				help(prog);
#ifndef HAVE___ATTRIBUTE__NORETURN
				break;
#endif

			case 'D':
				nut_debug_level++;
				break;

			case 'm': { /* var scope */
					char *m_arg, *s;

					monhost_ups_prev = monhost_ups_current;
					monhost_ups_current = xmalloc(sizeof(struct monhost_ups));
					if (monhost_ups_anchor == NULL)
						monhost_ups_anchor = monhost_ups_current;
					else
						monhost_ups_prev->next = monhost_ups_current;
					monhost_ups_current->next = NULL;
					monhost_len++;

					/* Be sure to not mangle original optarg, nor rely on its longevity */
					s = xstrdup(optarg);
					m_arg = s;
					/* splitting done in common loop below */
					monhost_ups_current->monhost = xstrdup(strsep(&m_arg, ","));
					if (!m_arg)
						fatalx(EXIT_FAILURE, "Argument '-m upsspec,logfile' requires exactly 2 components in the tuple");
#ifndef WIN32
					monhost_ups_current->logtarget = add_logfile(strsep(&m_arg, ","));
#else
					monhost_ups_current->logtarget = add_logfile(filter_path(strsep(&m_arg, ",")));
#endif
					if (m_arg) /* Had a third comma - also unexpected! */
						fatalx(EXIT_FAILURE, "Argument '-m upsspec,logfile' requires exactly 2 components in the tuple");
					free(s);
				} /* var scope */
				break;
			case 's':
				monhost = optarg;
				break;

			case 'l':
#ifndef WIN32
				logfn = optarg;
#else
				logfn = filter_path(optarg);
#endif
				break;

			case 'i':
				interval = atoi(optarg);
				break;

			case 'd':
				{	/* scoping */
					unsigned long ul = 0;
					if (str_to_ulong(optarg, &ul, 10)) {
#if (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_PUSH_POP) && ( (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TYPE_LIMITS) || (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TAUTOLOGICAL_CONSTANT_OUT_OF_RANGE_COMPARE) )
# pragma GCC diagnostic push
#endif
#ifdef HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TYPE_LIMITS
# pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#ifdef HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TAUTOLOGICAL_CONSTANT_OUT_OF_RANGE_COMPARE
# pragma GCC diagnostic ignored "-Wtautological-constant-out-of-range-compare"
#endif
						if (ul < SIZE_MAX)
							max_loops = (size_t)ul;
						else
							upslogx(LOG_ERR, "Invalid max loops");
#if (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_PUSH_POP) && ( (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TYPE_LIMITS) || (defined HAVE_PRAGMA_GCC_DIAGNOSTIC_IGNORED_TAUTOLOGICAL_CONSTANT_OUT_OF_RANGE_COMPARE) )
# pragma GCC diagnostic pop
#endif
					} else
						upslogx(LOG_ERR, "Invalid max loops");
				}
				break;

			case 'f':
				logformat = optarg;
				break;

			case 'u':
				user = optarg;
				break;

			case 'V':
				/* just show the version and optional
				 * CONFIG_FLAGS banner if available */
				print_banner_once(prog, 1);
				nut_report_config_flags();
				exit(EXIT_SUCCESS);

			case 'p':
				pidfilebase = optarg;
				break;

			case 'F':
				foreground = 1;
				break;

			case 'B':
				foreground = 0;
				break;

			default:
				fatalx(EXIT_FAILURE,
					"Error: unknown option -%c. Try -h for help.",
					(char)i);

		}
	}

	argc -= optind;
	argv += optind;

	/* not enough args for the old way? */
	if ((argc == 1) || (argc == 2))
		help(prog);

	/* see if it's being called in the old style - 3 or 4 args */

	/* <system> <logfn> <interval> [<format>] */

	if (argc >= 3) {
		monhost = argv[0];
#ifndef WIN32
		logfn = argv[1];
#else
		logfn = filter_path(argv[1]);
#endif
		interval = atoi(argv[2]);
	}

	if (argc >= 4) {
		/* read out the remaining argv entries to the format string */

		logformat = xmalloc(LARGEBUF);
		memset(logformat, '\0', LARGEBUF);

		for (i = 3; i < argc; i++)
			snprintfcat(logformat, LARGEBUF, "%s ", argv[i]);
	}

	/* Do we have the legacy single-monitoring CLI spec? */
	if (monhost || logfn) {
		/* Both data points must be defined, no defaults */
		if (!monhost)
			fatalx(EXIT_FAILURE, "No UPS defined for monitoring - use -s <system> when using -l <file>, or use -m <ups,logfile>");
		if (!logfn)
			fatalx(EXIT_FAILURE, "No filename defined for logging - use -l <file> when using -s <system>, or use -m <ups,logfile>");

		/* May be or not be NULL here: */
		monhost_ups_prev = monhost_ups_current;
		monhost_ups_current = xmalloc(sizeof(struct monhost_ups));
		if (monhost_ups_anchor == NULL) {
			/* Become the single-entry list */
			monhost_ups_anchor = monhost_ups_current;
		} else {
			/* Attach to existing list */
			monhost_ups_prev->next = monhost_ups_current;
		}
		monhost_ups_current->next = NULL;
		monhost_len++;

		/* splitting done in common loop below */
		monhost_ups_current->monhost = monhost;
		monhost_ups_current->logtarget = add_logfile(logfn);
	}

	/* shouldn't happen */
	if (!logformat)
		fatalx(EXIT_FAILURE, "No format defined - but this should be impossible");

	/* shouldn't happen */
	if (!monhost_len)
		fatalx(EXIT_FAILURE, "No UPS defined for monitoring - use -s <system> -l <logfile>, or use -m <ups,logfile>");

	/* Split the system specs in a common fashion for tuples and legacy args */
	for (monhost_ups_current = monhost_ups_anchor;
	     monhost_ups_current != NULL;
	     monhost_ups_current = monhost_ups_current->next
	) {
		if (upscli_splitname(monhost_ups_current->monhost, &(monhost_ups_current->upsname), &(monhost_ups_current->hostname), &(monhost_ups_current->port)) != 0) {
			fatalx(EXIT_FAILURE, "Error: invalid UPS definition.  Required format: upsname[@hostname[:port]]\n");
		}

		upsdebugx(1, "Checking parse of '%s' => '%s' '%s' '%" PRIu16 "'",
			NUT_STRARG(monhost_ups_current->monhost),
			NUT_STRARG(monhost_ups_current->upsname),
			NUT_STRARG(monhost_ups_current->hostname),
			monhost_ups_current->port
			);
	}

	/* Report the logged systems, open the log files as needed */
	for (monhost_ups_current = monhost_ups_anchor;
	     monhost_ups_current != NULL;
	     monhost_ups_current = monhost_ups_current->next
	) {
		printf("logging status of %s to %s (%is intervals)\n",
			monhost_ups_current->monhost,
			!strcmp(monhost_ups_current->logtarget->logfn, "-")
			? "stdout"
			: monhost_ups_current->logtarget->logfn,
			interval);
		if (upscli_splitname(monhost_ups_current->monhost, &(monhost_ups_current->upsname), &(monhost_ups_current->hostname), &(monhost_ups_current->port)) != 0) {
			fatalx(EXIT_FAILURE, "Error: invalid UPS definition.  Required format: upsname[@hostname[:port]]\n");
		}

		monhost_ups_current->ups = xmalloc(sizeof(UPSCONN_t));
		if (upscli_connect(monhost_ups_current->ups, monhost_ups_current->hostname, monhost_ups_current->port, UPSCLI_CONN_TRYSSL) < 0)
			fprintf(stderr, "Warning: initial connect failed: %s\n",
				upscli_strerror(monhost_ups_current->ups));

		/* we might have several systems logged into same file */
		if (monhost_ups_current->logtarget->logfile) {
			if (!strstr(logformat, "%UPSHOST%")) {
				if (monhost_ups_current->logtarget->logfile != stdout)
					upslogx(LOG_INFO, "NOTE: File %s is already receiving other logs",
						monhost_ups_current->logtarget->logfn);
				upslogx(LOG_INFO, "NOTE: Consider adding %%UPSHOST%% to the log formatting string");
			}
		} else {
			if (strcmp(monhost_ups_current->logtarget->logfn, "-") == 0)
				monhost_ups_current->logtarget->logfile = stdout;
			else
				monhost_ups_current->logtarget->logfile = fopen(monhost_ups_current->logtarget->logfn, "a");

			if (monhost_ups_current->logtarget->logfile == NULL)
				fatal_with_errno(EXIT_FAILURE, "could not open logfile %s", monhost_ups_current->logtarget->logfn);
		}
	}

	/* now drop root if we have it */
	new_uid = get_user_pwent(user);

	open_syslog(prog);

	if (foreground < 0) {
		if (nut_debug_level > 0 || get_logfile("-")) {
			foreground = 1;
		} else {
			foreground = 0;
		}
	}

	if (!foreground) {
		background();
	}

	setup_signals();

	writepid(pidfilebase);

	become_user(new_uid);

	compile_format();

	upsnotify(NOTIFY_STATE_READY_WITH_PID, NULL);

	while (exit_flag == 0) {
		upsnotify(NOTIFY_STATE_WATCHDOG, NULL);

		time(&now);

		if (nextpoll > now) {
			/* there is still time left, so sleep it off */
			sleep((unsigned int)(difftime(nextpoll, now)));
			nextpoll += interval;
		} else {
			/* we spent more time in polling than the interval allows */
			nextpoll = now + interval;
		}

		if (reopen_flag) {
			upsnotify(NOTIFY_STATE_RELOADING, NULL);
			upslogx(LOG_INFO, "Signal %d: reopening log file",
				reopen_flag);
			reopen_log();
			reopen_flag = 0;
			upsnotify(NOTIFY_STATE_READY, NULL);
		}

		for (monhost_ups_current = monhost_ups_anchor;
		     monhost_ups_current != NULL;
		     monhost_ups_current = monhost_ups_current->next
		) {
			ups = monhost_ups_current->ups;	/* XXX Not ideal */
			upsname = monhost_ups_current->upsname;	/* XXX Not ideal */
			monhost = monhost_ups_current->monhost;	/* XXX Not ideal */
			/* reconnect if necessary */
			if (upscli_fd(ups) < 0) {
				upscli_connect(ups, monhost_ups_current->hostname, monhost_ups_current->port, 0);
			}

			run_flist(monhost_ups_current);

			/* don't keep connection open if we don't intend to use it shortly */
			if (interval > 30) {
				upscli_disconnect(ups);
			}
		}

		if (max_loops > 0) {
			loop_count++;
			if (loop_count >= max_loops || loop_count > (SIZE_MAX - 1)) {
				upslogx(LOG_INFO, "%" PRIuSIZE " loops have elapsed", max_loops);
				exit_flag = 1;
			}
		}
	}

	upslogx(LOG_INFO, "Signal %d: exiting", exit_flag);
	upsnotify(NOTIFY_STATE_STOPPING, "Signal %d: exiting", exit_flag);

	for (monhost_ups_current = monhost_ups_anchor;
	     monhost_ups_current != NULL;
	     monhost_ups_current = monhost_ups_current->next
	) {
		/* we might have several systems logged into same file;
		 * take care to not close stdout though */
		if (monhost_ups_current->logtarget->logfile
		&&  monhost_ups_current->logtarget->logfile != stdout
		) {
			fclose(monhost_ups_current->logtarget->logfile);
			monhost_ups_current->logtarget->logfile = NULL;
		}

		upscli_disconnect(monhost_ups_current->ups);
	}

	exit(EXIT_SUCCESS);
}


/* Formal do_upsconf_args implementation to satisfy linker on AIX */
#if (defined NUT_PLATFORM_AIX)
void do_upsconf_args(char *upsname, char *var, char *val) {
        fatalx(EXIT_FAILURE, "INTERNAL ERROR: formal do_upsconf_args called");
}
#endif  /* end of #if (defined NUT_PLATFORM_AIX) */
