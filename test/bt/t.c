#include <sys/types.h>

#include <assert.h>
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

const char *progname;

int  load(void);
void progress(const char *, int);
int  read_check(void);
void setkd(int, void *, u_int32_t *, void *, u_int32_t *, int);
void usage(void);

int
main(int argc, char *argv[])
{
	u_int r;
	int ch, defkeys, defleafsize, defnodesize, i, ret, run_cnt;

	ret = 0;
	_malloc_options = "AJZ";

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	r = 0xdeadbeef ^ (u_int)time(NULL);
	defkeys = defleafsize = defnodesize = 1;
	while ((ch = getopt(argc, argv, "c:dk:l:n:R:r:s")) != EOF)
		switch (ch) {
		case 'c':
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
		case 's':
			stats = 1;
			break;
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	for (run_cnt = 1; runs == 0 || run_cnt < runs + 1; ++run_cnt) {
		(void)remove(MYDB);
		(void)remove(MYDUMP);
		(void)remove(MYPRINT);

		srand(r);

		/* If no number of keys, choose up to 1M. */
		if (defkeys)
			keys = rand() % 1000000;

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
		    "%s: %4d { -k %6d -l %6d -n %6d -R %#010lx }\n\t",
		    progname, run_cnt, keys, leafsize, nodesize, r);
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

int
load()
{
	WT_TOC *toc;
	DB *db;
	FILE *fp;

	__wt_single_thread_setup(progname, &toc, &db);

	db->set_errpfx(db, toc, progname);
	assert(db->env->set_cachesize(
	    db->env, toc, (u_int32_t)cachesize) == 0);
	assert(db->set_btree_pagesize(
	    db, toc, 0, (u_int32_t)nodesize, (u_int32_t)leafsize, 0) == 0);
	assert(db->open(db, toc, MYDB, 0660, WT_CREATE) == 0);

	assert(db->bulk_load(db, toc,
	    WT_DUPLICATES | WT_SORTED_INPUT, cb_bulk) == 0);

	progress("sync", 0);
	assert(db->sync(db, toc, 0) == 0);

	if (dumps) {
		progress("debug dump", 0);
		assert((fp = fopen(MYDUMP, "w")) != NULL);
		assert(db->dump(db, toc, fp, WT_DEBUG) == 0);
		assert(fclose(fp) == 0);

		progress("print dump", 0);
		assert((fp = fopen(MYPRINT, "w")) != NULL);
		assert(db->dump(db, toc, fp, WT_PRINTABLES) == 0);
		assert(fclose(fp) == 0);
	}

	progress("verify", 0);
	assert(db->verify(db, toc, 0) == 0);

	if (stats) {
		(void)printf("\nLoad statistics:\n");
		assert(db->stat_print(db, toc, stdout, 0) == 0);
		(void)printf("\n");
	}

	__wt_single_thread_teardown(progname, toc, db);

	return (0);
}

int
read_check()
{
	DB *db;
	DBT key, data;
	WT_TOC *toc;
	int cnt, last_cnt, ret;
	u_int32_t klen, dlen;
	char *kbuf, *dbuf;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

	__wt_single_thread_setup(progname, &toc, &db);

	db->set_errpfx(db, toc, progname);
	assert(db->env->set_cachesize(db->env, toc, (u_int32_t)cachesize) == 0);
	assert(db->open(db, toc, MYDB, 0660, WT_CREATE) == 0);

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
			db->err(db, ret, "get by key failed: {%.*s}",
			    (int)key.size, (char *)key.data);
			assert(0);
		}

		/* Get the key/data pair and check them. */
		setkd(cnt, &kbuf, &klen,
		    &dbuf, &dlen, atoi((char *)data.data + 11));
		if (key.size != klen || memcmp(kbuf, key.data, klen) ||
		    dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			db->errx(db,
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
			db->err(db, ret, "get by record failed: %d", cnt);
			assert(0);
		}

		/* Get the key/data pair and check them. */
		setkd(cnt, &kbuf, &klen,
		    &dbuf, &dlen, atoi((char *)data.data + 11));
		if (key.size != klen || memcmp(kbuf, key.data, klen) ||
		    dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			db->errx(db,
			    "get by record number %d:"
			    "\n\tkey: expected {%s}, got {%.*s}; "
			    "\n\tdata: expected {%s}, got {%.*s}",
			    cnt,
			    kbuf, (int)key.size, (char *)key.data,
			    dbuf, (int)data.size, (char *)data.data);
			assert(0);
		}
	}

	if (stats) {
		(void)printf("\nVerify statistics:\n");
		assert(db->stat_print(db, toc, stdout, 0) == 0);
		(void)printf("\n");
	}

	__wt_single_thread_teardown(progname, toc, db);
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
progress(const char *s, int i)
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
		len = snprintf(msg, sizeof(msg), "%s %d", s, i);

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
	    "usage: get [-ds] [-c cachesize] [-k keys] [-l leafsize] "
	    "[-n nodesize] [-R rand] [-r runs]\n");
	exit(1);
}
