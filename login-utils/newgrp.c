/* setgrp.c - by Michael Haardt. Set the gid if possible
 * Added a bit more error recovery/reporting - poe
 * Vesa Roukonen added code for asking password */

/* 1999-02-22 Arkadiusz Miśkiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 */

/*
 * This command is deprecated.  The utility is in maintenance mode,
 * meaning we keep them in source tree for backward compatibility
 * only.  Do not waste time making this command better, unless the
 * fix is about security or other very critical issue.
 *
 * See Documentation/deprecated.txt for more information.
 */

#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#ifdef HAVE_CRYPT_H
# include <crypt.h>
#endif

#ifdef HAVE_GETSGNAM
# include <gshadow.h>
#endif

#include "c.h"
#include "closestream.h"
#include "nls.h"
#include "pathnames.h"
#include "xalloc.h"

static char *xgetpass(FILE *input, const char *prompt)
{
	char *pass = NULL;
	struct termios saved, no_echo;
	const int fd = fileno(input);
	size_t dummy = 0;
	ssize_t len;

	fputs(prompt, stdout);
	if (isatty(fd)) {
		/* disable echo */
		tcgetattr(fd, &saved);
		no_echo = saved;
		no_echo.c_lflag &= ~ECHO;
		no_echo.c_lflag |= ECHONL;
		if (tcsetattr(fd, TCSANOW, &no_echo))
			err(EXIT_FAILURE, _("could not set terminal attributes"));
	}
	len = getline(&pass, &dummy, input);
	if (isatty(fd))
		/* restore terminal */
		if (tcsetattr(fd, TCSANOW, &saved))
			err(EXIT_FAILURE, _("could not set terminal attributes"));
	if (len < 0)
		err(EXIT_FAILURE, _("could not getline"));
	if (0 < len && *(pass + len - 1) == '\n')
		*(pass + len - 1) = '\0';
	return pass;
}

/* Ensure memory is set to value c without compiler optimization getting
 * into way that could happen with memset(3). */
static int memset_s(void *v, size_t sz, const int c)
{
	volatile unsigned char *p = v;

	if (v == NULL)
		return EINVAL;
	while (sz--)
		*p++ = c;
	return 0;
}

/* try to read password from gshadow */
static char *get_gshadow_pwd(char *groupname)
{
#ifdef HAVE_GETSGNAM
	struct sgrp *sgrp;

	sgrp = getsgnam(groupname);
	return sgrp ? xstrdup(sgrp->sg_passwd) : NULL;
#else
	char buf[BUFSIZ];
	char *pwd = NULL;
	FILE *f;

	if (groupname == NULL || *groupname == '\0')
		return NULL;

	f = fopen(_PATH_GSHADOW, "r");
	if (!f)
		return NULL;

	while (fgets(buf, sizeof buf, f)) {
		char *cp = strchr(buf, ':');
		if (!cp)
			/* any junk in gshadow? */
			continue;
		*cp = '\0';
		if (strcmp(buf, groupname) == 0) {
			if (cp - buf >= BUFSIZ)
				/* only group name on line */
				break;
			pwd = cp + 1;
			if ((cp = strchr(pwd, ':')) && pwd == cp + 1)
				/* empty password */
				pwd = NULL;
			else if (cp)
				*cp = '\0';
			break;
		}
	}
	fclose(f);
	return pwd ? xstrdup(pwd) : NULL;
#endif	/* HAVE_GETSGNAM */
}

static int allow_setgid(struct passwd *pe, struct group *ge)
{
	char **look;
	int notfound = 1;
	char *pwd, *xpwd;

	if (getuid() == 0)
		/* root may do anything */
		return TRUE;
	if (ge->gr_gid == pe->pw_gid)
		/* You can switch back to your default group */
		return TRUE;

	look = ge->gr_mem;
	while (*look && (notfound = strcmp(*look++, pe->pw_name))) ;

	if (!notfound)
		/* member of group => OK */
		return TRUE;

	/* Ask for password. Often there is no password in /etc/group, so
	 * contrary to login et al. we let an empty password mean the same
	 * as in /etc/passwd */

	/* check /etc/gshadow */
	if (!(pwd = get_gshadow_pwd(ge->gr_name)))
		pwd = ge->gr_passwd;

	if (pwd && *pwd && (xpwd = xgetpass(stdin, _("Password: ")))) {
		char *cbuf = crypt(xpwd, pwd);

		memset_s(xpwd, strlen(xpwd), 0);
		free(xpwd);
		if (!cbuf)
			warn(_("crypt failed"));
		else if (strcmp(pwd, cbuf) == 0)
			return TRUE;
	}

	/* default to denial */
	return FALSE;
}

static void __attribute__ ((__noreturn__)) usage(FILE * out)
{
	fprintf(out, USAGE_HEADER);
	fprintf(out, _(" %s <group>\n"), program_invocation_short_name);
	fprintf(out, USAGE_OPTIONS);
	fprintf(out, USAGE_HELP);
	fprintf(out, USAGE_VERSION);
	fprintf(out, USAGE_MAN_TAIL("newgrp(1)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	struct passwd *pw_entry;
	struct group *gr_entry;
	char *shell;
	char ch;
	static const struct option longopts[] = {
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((ch = getopt_long(argc, argv, "Vh", longopts, NULL)) != -1)
		switch (ch) {
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}

	if (!(pw_entry = getpwuid(getuid())))
		err(EXIT_FAILURE, _("who are you?"));

	shell = (pw_entry->pw_shell && *pw_entry->pw_shell ?
				pw_entry->pw_shell : _PATH_BSHELL);

	if (argc < 2) {
		if (setgid(pw_entry->pw_gid) < 0)
			err(EXIT_FAILURE, _("setgid failed"));
	} else {
		errno = 0;
		if (!(gr_entry = getgrnam(argv[1]))) {
			if (errno)
				err(EXIT_FAILURE, _("no such group"));
			else
				/* No group */
				errx(EXIT_FAILURE, _("no such group"));
		} else {
			if (allow_setgid(pw_entry, gr_entry)) {
				if (setgid(gr_entry->gr_gid) < 0)
					err(EXIT_FAILURE, _("setgid failed"));
			} else
				errx(EXIT_FAILURE, _("permission denied"));
		}
	}

	if (setuid(getuid()) < 0)
		err(EXIT_FAILURE, _("setuid failed"));

	fflush(stdout);
	fflush(stderr);
	execl(shell, shell, (char *)0);
	warn(_("failed to execute %s"), shell);
	fflush(stderr);

	return EXIT_FAILURE;
}
