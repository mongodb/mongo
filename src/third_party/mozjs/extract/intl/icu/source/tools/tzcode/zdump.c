/*
** This file is in the public domain, so clarified as of
** 2009-05-17 by Arthur David Olson.
*/

#include "version.h"

/*
** This code has been made independent of the rest of the time
** conversion package to increase confidence in the verification it provides.
** You can use this code to help in verifying other implementations.
**
** However, include private.h when debugging, so that it overrides
** time_t consistently with the rest of the package.
*/

#ifdef time_tz
# include "private.h"
#endif

#include "stdio.h"	/* for stdout, stderr, perror */
#include "string.h"	/* for strcpy */
#include "sys/types.h"	/* for time_t */
#include "time.h"	/* for struct tm */
#include "stdlib.h"	/* for exit, malloc, atoi */
#include "limits.h"	/* for CHAR_BIT, LLONG_MAX */
#include "ctype.h"	/* for isalpha et al. */

/* Enable extensions and modifications for ICU. */
#define ICU

#ifdef ICU
#include "dirent.h"
#include "sys/stat.h"
#endif

#ifndef isascii
#define isascii(x) 1
#endif /* !defined isascii */

/*
** Substitutes for pre-C99 compilers.
** Much of this section of code is stolen from private.h.
*/

#ifndef HAVE_STDINT_H
# define HAVE_STDINT_H \
    (199901 <= __STDC_VERSION__ || 2 < (__GLIBC__ + (0 < __GLIBC_MINOR__)))
#endif
#if HAVE_STDINT_H
# include "stdint.h"
#endif
#ifndef HAVE_INTTYPES_H
# define HAVE_INTTYPES_H HAVE_STDINT_H
#endif
#if HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#ifndef INT_FAST32_MAX
# if INT_MAX >> 31 == 0
typedef long int_fast32_t;
# else
typedef int int_fast32_t;
# endif
#endif

#ifndef INTMAX_MAX
# if defined LLONG_MAX || defined __LONG_LONG_MAX__
typedef long long intmax_t;
#  define strtoimax strtoll
#  define PRIdMAX "lld"
#  ifdef LLONG_MAX
#   define INTMAX_MAX LLONG_MAX
#  else
#   define INTMAX_MAX __LONG_LONG_MAX__
#  endif
# else
typedef long intmax_t;
#  define strtoimax strtol
#  define PRIdMAX "ld"
#  define INTMAX_MAX LONG_MAX
# endif
#endif


#ifndef ZDUMP_LO_YEAR
#define ZDUMP_LO_YEAR	(-500)
#endif /* !defined ZDUMP_LO_YEAR */

#ifndef ZDUMP_HI_YEAR
#define ZDUMP_HI_YEAR	2500
#endif /* !defined ZDUMP_HI_YEAR */

#ifndef MAX_STRING_LENGTH
#define MAX_STRING_LENGTH	1024
#endif /* !defined MAX_STRING_LENGTH */

#ifndef TRUE
#define TRUE		1
#endif /* !defined TRUE */

#ifndef FALSE
#define FALSE		0
#endif /* !defined FALSE */

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS	0
#endif /* !defined EXIT_SUCCESS */

#ifndef EXIT_FAILURE
#define EXIT_FAILURE	1
#endif /* !defined EXIT_FAILURE */

#ifndef SECSPERMIN
#define SECSPERMIN	60
#endif /* !defined SECSPERMIN */

#ifndef MINSPERHOUR
#define MINSPERHOUR	60
#endif /* !defined MINSPERHOUR */

#ifndef SECSPERHOUR
#define SECSPERHOUR	(SECSPERMIN * MINSPERHOUR)
#endif /* !defined SECSPERHOUR */

#ifndef HOURSPERDAY
#define HOURSPERDAY	24
#endif /* !defined HOURSPERDAY */

#ifndef EPOCH_YEAR
#define EPOCH_YEAR	1970
#endif /* !defined EPOCH_YEAR */

#ifndef TM_YEAR_BASE
#define TM_YEAR_BASE	1900
#endif /* !defined TM_YEAR_BASE */

#ifndef DAYSPERNYEAR
#define DAYSPERNYEAR	365
#endif /* !defined DAYSPERNYEAR */

#ifndef isleap
#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))
#endif /* !defined isleap */

#ifndef isleap_sum
/*
** See tzfile.h for details on isleap_sum.
*/
#define isleap_sum(a, b)	isleap((a) % 400 + (b) % 400)
#endif /* !defined isleap_sum */

#define SECSPERDAY	((int_fast32_t) SECSPERHOUR * HOURSPERDAY)
#define SECSPERNYEAR	(SECSPERDAY * DAYSPERNYEAR)
#define SECSPERLYEAR	(SECSPERNYEAR + SECSPERDAY)
#define SECSPER400YEARS	(SECSPERNYEAR * (intmax_t) (300 + 3)	\
			 + SECSPERLYEAR * (intmax_t) (100 - 3))

/*
** True if SECSPER400YEARS is known to be representable as an
** intmax_t.  It's OK that SECSPER400YEARS_FITS can in theory be false
** even if SECSPER400YEARS is representable, because when that happens
** the code merely runs a bit more slowly, and this slowness doesn't
** occur on any practical platform.
*/
enum { SECSPER400YEARS_FITS = SECSPERLYEAR <= INTMAX_MAX / 400 };

#ifndef HAVE_GETTEXT
#define HAVE_GETTEXT 0
#endif
#if HAVE_GETTEXT
#include "locale.h"	/* for setlocale */
#include "libintl.h"
#endif /* HAVE_GETTEXT */

#ifndef GNUC_or_lint
#ifdef lint
#define GNUC_or_lint
#else /* !defined lint */
#ifdef __GNUC__
#define GNUC_or_lint
#endif /* defined __GNUC__ */
#endif /* !defined lint */
#endif /* !defined GNUC_or_lint */

#if 2 < __GNUC__ || (__GNUC__ == 2 && 96 <= __GNUC_MINOR__)
# define ATTRIBUTE_PURE __attribute__ ((__pure__))
#else
# define ATTRIBUTE_PURE /* empty */
#endif

/*
** For the benefit of GNU folk...
** `_(MSGID)' uses the current locale's message library string for MSGID.
** The default is to use gettext if available, and use MSGID otherwise.
*/

#ifndef _
#if HAVE_GETTEXT
#define _(msgid) gettext(msgid)
#else /* !HAVE_GETTEXT */
#define _(msgid) msgid
#endif /* !HAVE_GETTEXT */
#endif /* !defined _ */

#ifndef TZ_DOMAIN
#define TZ_DOMAIN "tz"
#endif /* !defined TZ_DOMAIN */

extern char **	environ;
extern int	getopt(int argc, char * const argv[],
			const char * options);
extern char *	optarg;
extern int	optind;
extern char *	tzname[2];

/* The minimum and maximum finite time values.  */
static time_t const absolute_min_time =
  ((time_t) -1 < 0
   ? (time_t) -1 << (CHAR_BIT * sizeof (time_t) - 1)
   : 0);
static time_t const absolute_max_time =
  ((time_t) -1 < 0
   ? - (~ 0 < 0) - ((time_t) -1 << (CHAR_BIT * sizeof (time_t) - 1))
   : -1);
static size_t	longest;
static char *	progname;
static int	warned;

static char *	abbr(struct tm * tmp);
static void	abbrok(const char * abbrp, const char * zone);
static intmax_t	delta(struct tm * newp, struct tm * oldp) ATTRIBUTE_PURE;
static void	dumptime(const struct tm * tmp);
static time_t	hunt(char * name, time_t lot, time_t	hit);
static void	show(char * zone, time_t t, int v);
static const char *	tformat(void);
static time_t	yeartot(intmax_t y) ATTRIBUTE_PURE;
#ifdef ICU
typedef struct listentry {
	char *		name;
	struct listentry *	next;
} listentry;

static time_t	huntICU(char * name, time_t lot, time_t	hit, FILE *fp);
static void	dumptimeICU(FILE * fp, time_t t);
static void	showICU(FILE * fp, char * zone, time_t t1, time_t t2);
static int	getall(struct listentry ** namelist);
static void getzones(char * basedir, char * subdir, struct listentry ** last, int * count);
#endif

#ifndef TYPECHECK
#define my_localtime	localtime
#else /* !defined TYPECHECK */
static struct tm *
my_localtime(time_t *tp)
{
	register struct tm *	tmp;

	tmp = localtime(tp);
	if (tp != NULL && tmp != NULL) {
		struct tm	tm;
		register time_t	t;

		tm = *tmp;
		t = mktime(&tm);
		if (t != *tp) {
			(void) fflush(stdout);
			(void) fprintf(stderr, "\n%s: ", progname);
			(void) fprintf(stderr, tformat(), *tp);
			(void) fprintf(stderr, " ->");
			(void) fprintf(stderr, " year=%d", tmp->tm_year);
			(void) fprintf(stderr, " mon=%d", tmp->tm_mon);
			(void) fprintf(stderr, " mday=%d", tmp->tm_mday);
			(void) fprintf(stderr, " hour=%d", tmp->tm_hour);
			(void) fprintf(stderr, " min=%d", tmp->tm_min);
			(void) fprintf(stderr, " sec=%d", tmp->tm_sec);
			(void) fprintf(stderr, " isdst=%d", tmp->tm_isdst);
			(void) fprintf(stderr, " -> ");
			(void) fprintf(stderr, tformat(), t);
			(void) fprintf(stderr, "\n");
		}
	}
	return tmp;
}
#endif /* !defined TYPECHECK */

static void
abbrok(const char *const abbrp, const char *const zone)
{
	register const char *	cp;
	register const char *	wp;

	if (warned)
		return;
	cp = abbrp;
	wp = NULL;
	while (isascii((unsigned char) *cp) && isalpha((unsigned char) *cp))
		++cp;
	if (cp - abbrp == 0)
		wp = _("lacks alphabetic at start");
	else if (cp - abbrp < 3)
		wp = _("has fewer than 3 alphabetics");
	else if (cp - abbrp > 6)
		wp = _("has more than 6 alphabetics");
	if (wp == NULL && (*cp == '+' || *cp == '-')) {
		++cp;
		if (isascii((unsigned char) *cp) &&
			isdigit((unsigned char) *cp))
				if (*cp++ == '1' && *cp >= '0' && *cp <= '4')
					++cp;
		if (*cp != '\0')
			wp = _("differs from POSIX standard");
	}
	if (wp == NULL)
		return;
	(void) fflush(stdout);
	(void) fprintf(stderr,
		_("%s: warning: zone \"%s\" abbreviation \"%s\" %s\n"),
		progname, zone, abbrp, wp);
	warned = TRUE;
}

static void
usage(FILE * const stream, const int status)
{
	(void) fprintf(stream,
_("%s: usage: %s [--version] [--help] [-{vV}] [-{ct} [lo,]hi] zonename ...\n"
  "\n"
  "Report bugs to %s.\n"),
		       progname, progname, REPORT_BUGS_TO);
	exit(status);
}

int
main(int argc, char *argv[])
{
	register int		i;
	register int		vflag;
	register int		Vflag;
	register char *		cutarg;
	register char *		cuttimes;
	register time_t		cutlotime;
	register time_t		cuthitime;
	register char **	fakeenv;
	time_t			now;
	time_t			t;
	time_t			newt;
	struct tm		tm;
	struct tm		newtm;
	register struct tm *	tmp;
	register struct tm *	newtmp;
#ifdef ICU
	int		nextopt;
	char *	dirarg;
	int		aflag;
	int		iflag;
	listentry *	namelist = NULL;
	FILE *	fp = stdout;
#endif

	cutlotime = absolute_min_time;
	cuthitime = absolute_max_time;
#if HAVE_GETTEXT
	(void) setlocale(LC_ALL, "");
#ifdef TZ_DOMAINDIR
	(void) bindtextdomain(TZ_DOMAIN, TZ_DOMAINDIR);
#endif /* defined TEXTDOMAINDIR */
	(void) textdomain(TZ_DOMAIN);
#endif /* HAVE_GETTEXT */
	progname = argv[0];
	for (i = 1; i < argc; ++i)
		if (strcmp(argv[i], "--version") == 0) {
			(void) printf("zdump %s%s\n", PKGVERSION, TZVERSION);
			exit(EXIT_SUCCESS);
		} else if (strcmp(argv[i], "--help") == 0) {
			usage(stdout, EXIT_SUCCESS);
		}
	vflag = Vflag = 0;
	cutarg = cuttimes = NULL;
#ifdef ICU
	aflag = 0;
	iflag = 0;
	dirarg = NULL;
	for (;;)
		switch(getopt(argc, argv, "ac:d:it:vV")) {
		case 'a': aflag = 1; break;
		case 'c': cutarg = optarg; break;
		case 'd': dirarg = optarg; break;
		case 'i': iflag = 1; break;
		case 't': cuttimes = optarg; break;
		case 'v': vflag = 1; break;
		case 'V': Vflag = 1; break;
		case -1:
			if (! (optind == argc - 1 && strcmp(argv[optind], "=") == 0))
				goto arg_processing_done;
		/* Fall through.  */
		default:
			(void) fprintf(stderr,
				_("%s: usage is %s [ --version ] [ -a ] [ -v ] [ -V ] [ -i ] [ -c [loyear,]hiyear ] [ -t [lotime,]hitime] ][ -d dir ] [ zonename ... ]\n"),
				progname, progname);
			exit(EXIT_FAILURE);
		}
#else
	for (;;)
	  switch (getopt(argc, argv, "c:t:vV")) {
	  case 'c': cutarg = optarg; break;
	  case 't': cuttimes = optarg; break;
	  case 'v': vflag = 1; break;
	  case 'V': Vflag = 1; break;
	  case -1:
	    if (! (optind == argc - 1 && strcmp(argv[optind], "=") == 0))
	      goto arg_processing_done;
	    /* Fall through.  */
	  default:
	    usage(stderr, EXIT_FAILURE);
	  }
#endif
 arg_processing_done:;

#ifdef ICU
	if (dirarg != NULL) {
		DIR *	dp;
		/* create the output directory */
		mkdir(dirarg, 0777);
		if ((dp = opendir(dirarg)) == NULL) {
			fprintf(stderr, "cannot create the target directory");
			exit(EXIT_FAILURE);
		}
		closedir(dp);
	}
#endif

	if (vflag | Vflag) {
		intmax_t	lo;
		intmax_t	hi;
		char *loend, *hiend;
		register intmax_t cutloyear = ZDUMP_LO_YEAR;
		register intmax_t cuthiyear = ZDUMP_HI_YEAR;
		if (cutarg != NULL) {
			lo = strtoimax(cutarg, &loend, 10);
			if (cutarg != loend && !*loend) {
				hi = lo;
				cuthiyear = hi;
			} else if (cutarg != loend && *loend == ','
				   && (hi = strtoimax(loend + 1, &hiend, 10),
				       loend + 1 != hiend && !*hiend)) {
				cutloyear = lo;
				cuthiyear = hi;
			} else {
(void) fprintf(stderr, _("%s: wild -c argument %s\n"),
					progname, cutarg);
				exit(EXIT_FAILURE);
			}
		}
		if (cutarg != NULL || cuttimes == NULL) {
			cutlotime = yeartot(cutloyear);
			cuthitime = yeartot(cuthiyear);
		}
		if (cuttimes != NULL) {
			lo = strtoimax(cuttimes, &loend, 10);
			if (cuttimes != loend && !*loend) {
				hi = lo;
				if (hi < cuthitime) {
					if (hi < absolute_min_time)
						hi = absolute_min_time;
					cuthitime = hi;
				}
			} else if (cuttimes != loend && *loend == ','
				   && (hi = strtoimax(loend + 1, &hiend, 10),
				       loend + 1 != hiend && !*hiend)) {
				if (cutlotime < lo) {
					if (absolute_max_time < lo)
						lo = absolute_max_time;
					cutlotime = lo;
				}
				if (hi < cuthitime) {
					if (hi < absolute_min_time)
						hi = absolute_min_time;
					cuthitime = hi;
				}
			} else {
				(void) fprintf(stderr,
					_("%s: wild -t argument %s\n"),
					progname, cuttimes);
				exit(EXIT_FAILURE);
			}
		}
	}

#ifdef ICU
	if (aflag) {
		/* get all available zones */
		char ** fakeargv;
		int i;
		int count;

		count = getall(&namelist);
		fakeargv = (char **) malloc((size_t) (argc + count) * sizeof *argv);
		/*
		if ((fakeargv = (char **) malloc((size_t) (argc + count) * sizeof *argv)) == NULL) {
			exit(EXIT_FAILURE);
		}
		*/
		for (i = 0; i < argc; i++) {
			fakeargv[i] = argv[i];
		}
		for (i = 0; i < count; i++) {
			fakeargv[i + argc] = namelist->name;
			namelist = namelist->next;
		}
		argv = fakeargv;
		argc += count;
	}
#endif
	(void) time(&now);
	longest = 0;
	for (i = optind; i < argc; ++i)
		if (strlen(argv[i]) > longest)
			longest = strlen(argv[i]);
	{
		register int	from;
		register int	to;

		for (i = 0; environ[i] != NULL; ++i)
			continue;
		fakeenv = malloc((i + 2) * sizeof *fakeenv);
		if (fakeenv == NULL
		    || (fakeenv[0] = malloc(longest + 4)) == NULL) {
					(void) perror(progname);
					exit(EXIT_FAILURE);
		}
		to = 0;
		(void) strcpy(fakeenv[to++], "TZ=");
		for (from = 0; environ[from] != NULL; ++from)
			if (strncmp(environ[from], "TZ=", 3) != 0)
				fakeenv[to++] = environ[from];
		fakeenv[to] = NULL;
		environ = fakeenv;
	}
	for (i = optind; i < argc; ++i) {
		static char	buf[MAX_STRING_LENGTH];

		(void) strcpy(&fakeenv[0][3], argv[i]);
		if (! (vflag | Vflag)) {
			show(argv[i], now, FALSE);
			continue;
		}
#ifdef ICU
		fp = NULL;
		if (iflag) {
			if (dirarg == NULL) {
				/* we want to display a zone name here */
				if (i != optind) {
					printf("\n");
				}
				printf("ZONE: %s\n", argv[i]);
			} else {
				int		zstart;
				char	path[FILENAME_MAX + 1];
				strcpy(path, dirarg);
				strcat(path, "/");
				zstart = strlen(path);
				strcat(path, argv[i]);
				/* replace '/' with '-' */
				while(path[++zstart] != 0) {
					if (path[zstart] == '/') {
						path[zstart] = '-';
					}
				}
				if ((fp = fopen(path, "w")) == NULL) {
					fprintf(stderr, "cannot create output file %s\n", path);
					exit(EXIT_FAILURE);
				}
			}
		}
#endif
		warned = FALSE;
		t = absolute_min_time;
#ifdef ICU
		/* skip displaying info for the lowest time, which is actually not
		 * a transition when -i option is set */
		if (!iflag) {
#endif
		if (!Vflag) {
			show(argv[i], t, TRUE);
			t += SECSPERDAY;
			show(argv[i], t, TRUE);
		}
#ifdef ICU
		}
#endif
		if (t < cutlotime)
			t = cutlotime;
		tmp = my_localtime(&t);
		if (tmp != NULL) {
			tm = *tmp;
			(void) strncpy(buf, abbr(&tm), (sizeof buf) - 1);
		}
		for ( ; ; ) {
			newt = (t < absolute_max_time - SECSPERDAY / 2
				? t + SECSPERDAY / 2
				: absolute_max_time);
			if (cuthitime <= newt)
				break;
			newtmp = localtime(&newt);
			if (newtmp != NULL)
				newtm = *newtmp;
#ifdef ICU
			if (iflag) {
				/* We  do not want to capture transitions just for
				 * abbreviated zone name changes */
				if ((tmp == NULL || newtmp == NULL) ? (tmp != newtmp) :
					(delta(&newtm, &tm) != (newt - t) ||
					newtm.tm_isdst != tm.tm_isdst)) {
						newt = huntICU(argv[i], t, newt, fp);
						newtmp = localtime(&newt);
						if (newtmp != NULL) {
							newtm = *newtmp;
							(void) strncpy(buf,
								abbr(&newtm),
								(sizeof buf) - 1);
						}
				}
			} else {
#endif
			if ((tmp == NULL || newtmp == NULL) ? (tmp != newtmp) :
				(delta(&newtm, &tm) != (newt - t) ||
				newtm.tm_isdst != tm.tm_isdst ||
				strcmp(abbr(&newtm), buf) != 0)) {
					newt = hunt(argv[i], t, newt);
					newtmp = localtime(&newt);
					if (newtmp != NULL) {
						newtm = *newtmp;
						(void) strncpy(buf,
							abbr(&newtm),
							(sizeof buf) - 1);
					}
			}
#ifdef ICU
			}
#endif
			t = newt;
			tm = newtm;
			tmp = newtmp;
		}
#ifdef ICU
		if (!iflag) {
		/* skip displaying info for the highest time, which is actually not
		 * a transition when -i option is used*/
#endif
		if (!Vflag) {
			t = absolute_max_time;
			t -= SECSPERDAY;
			show(argv[i], t, TRUE);
			t += SECSPERDAY;
			show(argv[i], t, TRUE);
		}
#ifdef ICU
		}
		/* close file */
		if (fp != NULL) {
			fclose(fp);
		}
#endif
	}
	if (fflush(stdout) || ferror(stdout)) {
		(void) fprintf(stderr, "%s: ", progname);
		(void) perror(_("Error writing to standard output"));
		exit(EXIT_FAILURE);
	}
#ifdef ICU
	if (aflag) {
		struct listentry *	entry = namelist;
		struct listentry *	next;
		while (entry != NULL) {
			free(entry->name);
			next = entry->next;
			free(entry);
			entry = next;
		}
	}
#endif
	exit(EXIT_SUCCESS);
	/* If exit fails to exit... */
	return EXIT_FAILURE;
}

static time_t
yeartot(const intmax_t y)
{
	register intmax_t	myy, seconds, years;
	register time_t		t;

	myy = EPOCH_YEAR;
	t = 0;
	while (myy < y) {
		if (SECSPER400YEARS_FITS && 400 <= y - myy) {
			intmax_t diff400 = (y - myy) / 400;
			if (INTMAX_MAX / SECSPER400YEARS < diff400)
				return absolute_max_time;
			seconds = diff400 * SECSPER400YEARS;
			years = diff400 * 400;
                } else {
			seconds = isleap(myy) ? SECSPERLYEAR : SECSPERNYEAR;
			years = 1;
		}
		myy += years;
		if (t > absolute_max_time - seconds)
			return absolute_max_time;
		t += seconds;
	}
	while (y < myy) {
		if (SECSPER400YEARS_FITS && y + 400 <= myy && myy < 0) {
			intmax_t diff400 = (myy - y) / 400;
			if (INTMAX_MAX / SECSPER400YEARS < diff400)
				return absolute_min_time;
			seconds = diff400 * SECSPER400YEARS;
			years = diff400 * 400;
		} else {
			seconds = isleap(myy - 1) ? SECSPERLYEAR : SECSPERNYEAR;
			years = 1;
		}
		myy -= years;
		if (t < absolute_min_time + seconds)
			return absolute_min_time;
		t -= seconds;
	}
	return t;
}

static time_t
hunt(char *name, time_t lot, time_t hit)
{
	time_t			t;
	struct tm		lotm;
	register struct tm *	lotmp;
	struct tm		tm;
	register struct tm *	tmp;
	char			loab[MAX_STRING_LENGTH];

	lotmp = my_localtime(&lot);
	if (lotmp != NULL) {
		lotm = *lotmp;
		(void) strncpy(loab, abbr(&lotm), (sizeof loab) - 1);
	}
	for ( ; ; ) {
		time_t diff = hit - lot;
		if (diff < 2)
			break;
		t = lot;
		t += diff / 2;
		if (t <= lot)
			++t;
		else if (t >= hit)
			--t;
		tmp = my_localtime(&t);
		if (tmp != NULL)
			tm = *tmp;
		if ((lotmp == NULL || tmp == NULL) ? (lotmp == tmp) :
			(delta(&tm, &lotm) == (t - lot) &&
			tm.tm_isdst == lotm.tm_isdst &&
			strcmp(abbr(&tm), loab) == 0)) {
				lot = t;
				lotm = tm;
				lotmp = tmp;
		} else	hit = t;
	}
	show(name, lot, TRUE);
	show(name, hit, TRUE);
	return hit;
}

/*
** Thanks to Paul Eggert for logic used in delta.
*/

static intmax_t
delta(struct tm * newp, struct tm *oldp)
{
	register intmax_t	result;
	register int		tmy;

	if (newp->tm_year < oldp->tm_year)
		return -delta(oldp, newp);
	result = 0;
	for (tmy = oldp->tm_year; tmy < newp->tm_year; ++tmy)
		result += DAYSPERNYEAR + isleap_sum(tmy, TM_YEAR_BASE);
	result += newp->tm_yday - oldp->tm_yday;
	result *= HOURSPERDAY;
	result += newp->tm_hour - oldp->tm_hour;
	result *= MINSPERHOUR;
	result += newp->tm_min - oldp->tm_min;
	result *= SECSPERMIN;
	result += newp->tm_sec - oldp->tm_sec;
	return result;
}

static void
show(char *zone, time_t t, int v)
{
	register struct tm *	tmp;

	(void) printf("%-*s  ", (int) longest, zone);
	if (v) {
		tmp = gmtime(&t);
		if (tmp == NULL) {
			(void) printf(tformat(), t);
		} else {
			dumptime(tmp);
			(void) printf(" UT");
		}
		(void) printf(" = ");
	}
	tmp = my_localtime(&t);
	dumptime(tmp);
	if (tmp != NULL) {
		if (*abbr(tmp) != '\0')
			(void) printf(" %s", abbr(tmp));
		if (v) {
			(void) printf(" isdst=%d", tmp->tm_isdst);
#ifdef TM_GMTOFF
			(void) printf(" gmtoff=%ld", tmp->TM_GMTOFF);
#endif /* defined TM_GMTOFF */
		}
	}
	(void) printf("\n");
	if (tmp != NULL && *abbr(tmp) != '\0')
		abbrok(abbr(tmp), zone);
}

static char *
abbr(struct tm *tmp)
{
	register char *	result;
	static char	nada;

	if (tmp->tm_isdst != 0 && tmp->tm_isdst != 1)
		return &nada;
	result = tzname[tmp->tm_isdst];
	return (result == NULL) ? &nada : result;
}

/*
** The code below can fail on certain theoretical systems;
** it works on all known real-world systems as of 2004-12-30.
*/

static const char *
tformat(void)
{
	if (0 > (time_t) -1) {		/* signed */
		if (sizeof (time_t) == sizeof (intmax_t))
			return "%"PRIdMAX;
		if (sizeof (time_t) > sizeof (long))
			return "%lld";
		if (sizeof (time_t) > sizeof (int))
			return "%ld";
		return "%d";
	}
#ifdef PRIuMAX
	if (sizeof (time_t) == sizeof (uintmax_t))
		return "%"PRIuMAX;
#endif
	if (sizeof (time_t) > sizeof (unsigned long))
		return "%llu";
	if (sizeof (time_t) > sizeof (unsigned int))
		return "%lu";
	return "%u";
}

static void
dumptime(register const struct tm *timeptr)
{
	static const char	wday_name[][3] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	static const char	mon_name[][3] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	register const char *	wn;
	register const char *	mn;
	register int		lead;
	register int		trail;

	if (timeptr == NULL) {
		(void) printf("NULL");
		return;
	}
	/*
	** The packaged versions of localtime and gmtime never put out-of-range
	** values in tm_wday or tm_mon, but since this code might be compiled
	** with other (perhaps experimental) versions, paranoia is in order.
	*/
	if (timeptr->tm_wday < 0 || timeptr->tm_wday >=
		(int) (sizeof wday_name / sizeof wday_name[0]))
			wn = "???";
	else		wn = wday_name[timeptr->tm_wday];
	if (timeptr->tm_mon < 0 || timeptr->tm_mon >=
		(int) (sizeof mon_name / sizeof mon_name[0]))
			mn = "???";
	else		mn = mon_name[timeptr->tm_mon];
	(void) printf("%.3s %.3s%3d %.2d:%.2d:%.2d ",
		wn, mn,
		timeptr->tm_mday, timeptr->tm_hour,
		timeptr->tm_min, timeptr->tm_sec);
#define DIVISOR	10
	trail = timeptr->tm_year % DIVISOR + TM_YEAR_BASE % DIVISOR;
	lead = timeptr->tm_year / DIVISOR + TM_YEAR_BASE / DIVISOR +
		trail / DIVISOR;
	trail %= DIVISOR;
	if (trail < 0 && lead > 0) {
		trail += DIVISOR;
		--lead;
	} else if (lead < 0 && trail > 0) {
		trail -= DIVISOR;
		++lead;
	}
	if (lead == 0)
		(void) printf("%d", trail);
	else	(void) printf("%d%d", lead, ((trail < 0) ? -trail : trail));
}

#ifdef ICU
static time_t
huntICU(char *name, time_t lot, time_t hit, FILE * fp)
{
	time_t			t;
	long			diff;
	struct tm		lotm;
	register struct tm *	lotmp;
	struct tm		tm;
	register struct tm *	tmp;
	char			loab[MAX_STRING_LENGTH];

	lotmp = my_localtime(&lot);
	if (lotmp != NULL) {
		lotm = *lotmp;
		(void) strncpy(loab, abbr(&lotm), (sizeof loab) - 1);
	}
	for ( ; ; ) {
		diff = (long) (hit - lot);
		if (diff < 2)
			break;
		t = lot;
		t += diff / 2;
		if (t <= lot)
			++t;
		else if (t >= hit)
			--t;
		tmp = my_localtime(&t);
		if (tmp != NULL)
			tm = *tmp;
		/* We  do not want to capture transitions just for
		 * abbreviated zone name changes */
		if ((lotmp == NULL || tmp == NULL) ? (lotmp == tmp) :
			(delta(&tm, &lotm) == (t - lot) &&
			tm.tm_isdst == lotm.tm_isdst)) {
				lot = t;
				lotm = tm;
				lotmp = tmp;
		} else	hit = t;
	}
	showICU(fp, name, lot, hit);
	return hit;
}

static void showICU(FILE * fp, char *zone, time_t t1, time_t t2)
{
	if (fp == NULL) {
		fp = stdout;
	}
	dumptimeICU(fp, t1);
	fprintf(fp, " > ");
	dumptimeICU(fp, t2);
	fprintf(fp, "\n");
}

static void dumptimeICU(FILE * fp, time_t t)
{
	static const char	wday_name[][3] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	struct tm gmt;
	struct tm loc;
	register int		lead;
	register int		trail;
	long	offset;
	long	hour, min, sec;

	loc = *my_localtime(&t);

	trail = loc.tm_year % DIVISOR + TM_YEAR_BASE % DIVISOR;
	lead = loc.tm_year / DIVISOR + TM_YEAR_BASE / DIVISOR + trail / DIVISOR;
	trail %= DIVISOR;
	if (trail < 0 && lead > 0) {
		trail += DIVISOR;
		--lead;
	} else if (lead < 0 && trail > 0) {
		trail -= DIVISOR;
		++lead;
	}

	fprintf(fp, "%04d-%02d-%02d", lead * DIVISOR + trail, loc.tm_mon + 1, loc.tm_mday);
	fprintf(fp, " %.3s ", wday_name[loc.tm_wday]);
	fprintf(fp, "%02d:%02d:%02d", loc.tm_hour, loc.tm_min, loc.tm_sec);

	gmt = *gmtime(&t);
	offset = delta(&loc, &gmt);
	if (offset < 0) {
		offset = -offset;
		fprintf(fp, "-");
	} else {
		fprintf(fp, "+");
	}

	sec = offset % 60;
	offset = (offset - sec) / 60;
	min = offset % 60;
	hour = offset / 60;
	
	fprintf(fp, "%02ld", hour);
	fprintf(fp, "%02ld", min);
	fprintf(fp, "%02ld", sec);
	fprintf(fp, "[DST=%d]", loc.tm_isdst);
}

static int getall(struct listentry ** namelist) {
	int count = 0;
	struct listentry dummyentry;
	struct listentry *	last = &dummyentry;

	getzones(TZDIR, NULL, &last, &count);
	if (count > 0) {
		*namelist = dummyentry.next;
	}

	return count;
}

static void getzones(char * basedir, char * relpath, struct listentry ** last, int * count) {
	char	path[FILENAME_MAX + 1];
	struct	dirent *	dir;
	DIR *	dp;

	strcpy(path, basedir);
	if (relpath != NULL) {
		strcat(path, "/");
		strcat(path, relpath);
	}

	if ((dp = opendir(path)) == NULL) {
		/* file */
		if (strstr(relpath, ".tab") == NULL && strcmp(relpath, "Etc/Unknown") != 0) {
			char *		pzonename;
			listentry *	pentry;

			if ((pzonename = malloc(strlen(relpath) + 1)) == NULL) {
				exit(EXIT_FAILURE);
			}
			strcpy(pzonename, relpath);
		
			if ((pentry = malloc(sizeof(listentry))) == NULL) {
				exit(EXIT_FAILURE);
			}

			pentry->name = pzonename;
			pentry->next = NULL;
			(*last)->next = pentry;
			*last = pentry;
			(*count)++;
		}
	} else {
		/* directory */
		while ((dir = readdir(dp)) != NULL) {
			char	subpath[FILENAME_MAX + 1];

			if (strcmp(dir->d_name, ".") == 0
				|| strcmp(dir->d_name, "..") == 0) {
				continue;
			}
			if (relpath != NULL) {
				strcpy(subpath, relpath);
				strcat(subpath, "/");
				strcat(subpath, dir->d_name);
			} else {
				strcpy(subpath, dir->d_name);
			}
			getzones(basedir, subpath, last, count);
		}
		closedir(dp);
	}
}
#endif
