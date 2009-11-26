#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wiredtiger.h"

#define	MYDB	"a.db"
#define	MYDUMP	"a.debug"
#define	MYPRINT	"a.print"

int cachesize = 20;				/* Cache size: default 25MB */
int dumps = 0;					/* Dump database */
int keys = 0;					/* Keys: default 5M */
int keys_cnt = 0;				/* Count of keys in this run */
int leafsize = 0;				/* Leaf page size */
int nodesize = 0;				/* Node page size */
int runs = 0;					/* Runs: default forever */
int stats = 0;					/* Show statistics */
int verbose = 0;				/* Verbose debugging */

const char *progname;

FILE *logfp;
char *logfile;

void	track(const char *, u_int32_t);
int	load(void);
void	progress(const char *, u_int32_t);
int	read_check(void);
void	setkd(int, void *, u_int32_t *, void *, u_int32_t *, int);
void	usage(void);

int
main(int argc, char *argv[])
{
	u_int r;
	int ch, i, ret, run_cnt;
	int defcachesize, defkeys, defleafsize, defnodesize;

	ret = 0;
	_malloc_options = "AJZ";

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	r = 0xdeadbeef ^ (u_int)time(NULL);
	defcachesize = defkeys = defleafsize = defnodesize =  1;
	while ((ch = getopt(argc, argv, "c:dk:L:l:n:R:r:Sv")) != EOF)
		switch (ch) {
		case 'c':
			defcachesize = 0;
			cachesize = atoi(optarg);
			break;
		case 'd':
			dumps = 1;
			break;
		case 'k':
			defkeys = 0;
			keys = atoi(optarg);
			break;
		case 'l':
			defleafsize = 0;
			leafsize = atoi(optarg);
			break;
		case 'L':
			logfile = optarg;
			break;
		case 'n':
			defnodesize = 0;
			nodesize = atoi(optarg);
			break;
		case 'R':
			r = (u_int)strtoul(optarg, NULL, 0);
			break;
		case 'r':
			runs = atoi(optarg);
			break;
		case 'S':
			stats = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	printf("get: process %lu\n", (u_long)getpid());
	for (run_cnt = 1; runs == 0 || run_cnt < runs + 1; ++run_cnt) {
		(void)remove(MYDB);
		(void)remove(MYDUMP);
		(void)remove(MYPRINT);

		if (logfp != NULL)
			(void)fclose(logfp);
		if (logfile != NULL && (logfp = fopen(logfile, "w")) == NULL) {
			fprintf(stderr, "%s: %s\n", logfile, strerror(errno));
			return (EXIT_FAILURE);
		}

		srand(r);

		/* If no number of keys, choose up to 5M. */
		if (defkeys)
			keys = rand() % 5000000;

		/* If no cachesize given, choose between 2M and 20M. */
		if (defcachesize)
			cachesize = 2 + rand() % 18;

		/*
		 * If no leafsize or nodesize given, choose between 512B and
		 * 128KB.
		 */
		if (defleafsize)
			for (leafsize = 512, i = rand() % 9; i > 0; --i)
				leafsize *= 2;
		if (defnodesize)
			for (nodesize = 512, i = rand() % 9; i > 0; --i)
				nodesize *= 2;

		(void)printf(
		    "%s: %4d { -c %2d -k %7d -l %6d -n %6d -R %#010lx }\n\t",
		    progname, run_cnt, cachesize, keys, leafsize, nodesize, r);
		(void)fflush(stdout);

		keys_cnt = 0;
		if ((ret = load()) != 0 || (ret = read_check()) != 0) {
			(void)printf("FAILED!\n");
			break;
		}
		progress(NULL, 0);
		(void)printf("OK\r");

		r = rand() ^ time(NULL);
	}

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

int
cb_bulk(DB *db, DBT **keyp, DBT **datap)
{
	static DBT key, data;

	if (++keys_cnt == keys + 1)
		return (1);

	if (keys_cnt % 1000 == 0)
		progress("load", keys_cnt);

	setkd(keys_cnt, &key.data, &key.size, &data.data, &data.size, 0);
    
	*keyp = &key;
	*datap = &data;

	return (0);
}

void
track(const char *s, u_int32_t i)
{
	progress(s, i);
}

int
load()
{
	pthread_t tid;
	ENV *env;
	DB *db;
	FILE *fp;

	assert(wiredtiger_simple_setup(progname, &db) == 0);
	env = db->env;

	if (logfp != NULL) {
		env->msgfile_set(env, logfp);
		env->verbose_set(env, 0);
	}
	db->errpfx_set(db, progname);
	assert(env->cachesize_set(env, (u_int32_t)cachesize) == 0);
	assert(db->btree_pagesize_set(
	    db, 0, (u_int32_t)nodesize, (u_int32_t)leafsize, 0) == 0);
	assert(db->open(db, MYDB, 0660, WT_CREATE) == 0);

	assert(db->bulk_load(db,
	    WT_DUPLICATES | WT_SORTED_INPUT, cb_bulk) == 0);

	assert(db->sync(db, track, 0) == 0);

	if (dumps) {
		progress("debug dump", 0);
		assert((fp = fopen(MYDUMP, "w")) != NULL);
		assert(db->dump(db, fp, WT_DEBUG) == 0);
		assert(fclose(fp) == 0);

		progress("print dump", 0);
		assert((fp = fopen(MYPRINT, "w")) != NULL);
		assert(db->dump(db, fp, WT_PRINTABLES) == 0);
		assert(fclose(fp) == 0);
	}

	assert(db->verify(db, track, 0) == 0);

	if (stats) {
		(void)printf("\nLoad statistics:\n");
		assert(env->stat_print(env, stdout, 0) == 0);
	}

	assert(wiredtiger_simple_teardown(progname, db) == 0);

	return (0);
}

int
read_check()
{
	DB *db;
	DBT key, data;
	ENV *env;
	WT_TOC *toc;
	int cnt, last_cnt, ret;
	u_int32_t klen, dlen;
	char *kbuf, *dbuf;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

	assert(wiredtiger_simple_setup(progname, &db) == 0);
	env = db->env;

	if (logfp != NULL) {
		env->msgfile_set(env, logfp);
		env->verbose_set(env, 0);
	}
	db->errpfx_set(db, progname);
	assert(env->cachesize_set(env, (u_int32_t)cachesize) == 0);
	assert(db->open(db, MYDB, 0660, WT_CREATE) == 0);
	assert(env->toc(env, 0, &toc) == 0);

	/* Check a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < keys;) {
		cnt += rand() % 37 + 1;
		if (cnt > keys)
			cnt = keys;
		if (cnt - last_cnt > 1000) {
			progress("key read-check", cnt);
			last_cnt = cnt;
		}

		/* Get the key and look it up. */
		setkd(cnt, &key.data, &key.size, NULL, NULL, 0);
		if ((ret = db->get(db, toc, &key, NULL, &data, 0)) != 0) {
			env->err(env, ret, "get by key failed: {%.*s}",
			    (int)key.size, (char *)key.data);
			assert(0);
		}

		/* Get the key/data pair and check them. */
		setkd(cnt, &kbuf, &klen,
		    &dbuf, &dlen, atoi((char *)data.data + 11));
		if (key.size != klen || memcmp(kbuf, key.data, klen) ||
		    dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			env->errx(env,
			    "get by key:"
			    "\n\tkey: expected {%s}, got {%.*s}; "
			    "\n\tdata: expected {%s}, got {%.*s}",
			    cnt,
			    kbuf, (int)key.size, (char *)key.data,
			    dbuf, (int)data.size, (char *)data.data);
			assert(0);
		}
	}

	/* Check a random subset of the records using the record number. */
	for (last_cnt = cnt = 0; cnt < keys;) {
		cnt += rand() % 37 + 1;
		if (cnt > keys)
			cnt = keys;
		if (cnt - last_cnt > 1000) {
			progress("recno read-check", cnt);
			last_cnt = cnt;
		}

		/* Look up the key/data pair by record number. */
		if ((ret = db->get_recno(
		    db, toc, (u_int64_t)cnt, &key, NULL, &data, 0)) != 0) {
			env->err(env, ret, "get by record failed: %d", cnt);
			assert(0);
		}

		/* Get the key/data pair and check them. */
		setkd(cnt, &kbuf, &klen,
		    &dbuf, &dlen, atoi((char *)data.data + 11));
		if (key.size != klen || memcmp(kbuf, key.data, klen) ||
		    dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			env->errx(env,
			    "get by record number %d:"
			    "\n\tkey: expected {%s}, got {%.*s}; "
			    "\n\tdata: expected {%s}, got {%.*s}",
			    cnt,
			    kbuf, (int)key.size, (char *)key.data,
			    dbuf, (int)data.size, (char *)data.data);
			assert(0);
		}
	}

	assert(toc->close(toc, 0) == 0);
	assert(db->close(db, 0) == 0);

	if (stats) {
		(void)printf("\nVerify statistics:\n");
		assert(env->stat_print(env, stdout, 0) == 0);
	}

	assert(wiredtiger_simple_teardown(progname, NULL) == 0);

	return (0);

}

void
setkd(int cnt,
    void *kbufp, u_int32_t *klenp, void *dbufp, u_int32_t *dlenp, int dlen)
{
	static char kbuf[64], dbuf[512];
	int klen;

	/* The key is a 10-digit length. */
	klen = snprintf(kbuf, sizeof(kbuf), "%010d", cnt);
	*(char **)kbufp = kbuf;
	*klenp = (u_int32_t)klen;

	/* We only want the key, to start. */
	if (dbufp == NULL)
		return;

	/*
	 * The data item is a 10-digit key, a '*', a 10-digit length, a '*',
	 * a random number of 'a' characters and a trailing '*'.
	 *
	 * If we're passed a len, we're re-creating a previously created
	 * data item, use the length; otherwise, generate a new one.
	 */
	memset(dbuf, 'a', sizeof(dbuf));
	if (dlen == 0)
		dlen = rand() % 450 + 30;
	dbuf[snprintf(dbuf, sizeof(dbuf), "%010d*%010d*", cnt, dlen)] = 'a';
	dbuf[dlen - 1] = '*';
	*(char **)dbufp = dbuf;
	*dlenp = (u_int32_t)dlen;
}

void
progress(const char *s, u_int32_t i)
{
	static int maxlen = 0;
	int len;
	char *p, msg[128];

	if (!isatty(0))
		return;

	if (s == NULL)
		len = 0;
	else if (i == 0)
		len = snprintf(msg, sizeof(msg), "%s", s);
	else
		len = snprintf(msg, sizeof(msg), "%s %lu", s, (u_int32_t)i);

	for (p = msg + len; len < maxlen; ++len)
		*p++ = ' ';
	maxlen = len;
	for (; len > 0; --len)
		*p++ = '\b';
	*p = '\0';
	(void)printf("%s", msg);
	(void)fflush(stdout);
}

void
usage()
{
	(void)fprintf(stderr,
	    "usage: get [-dSv] [-c cachesize]\n    [-k keys] [-L logfile] "
	    "[-l leafsize] [-n nodesize] [-R rand] [-r runs]\n");
	exit(1);
}
