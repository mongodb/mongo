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

int cachesize = 0;				/* Cache size */
int fixed_len = 0;				/* Fixed length */
int huffman = 0;				/* Compress */
int keys = 0;					/* Keys */
int keys_cnt = 0;				/* Count of keys loaded */
int leafsize = 0;				/* Leaf page size */
int nodesize = 0;				/* Node page size */
int repeat_compress = 0;			/* Repeat compression */
int runs = 0;					/* Runs: default forever */
int verbose = 0;				/* Verbose debugging */

int op_dump = 0;				/* Dump database */
int op_reopen = 0;				/* Sync and reopen database */
int op_verify = 0;				/* Verify database */

enum {						/* Statistics */
    STAT_NONE, STAT_ALL, STAT_LOAD, STAT_READ, STAT_WRITE } stat_op = STAT_NONE;
enum {						/* Database type */
    TYPE_COLUMN_FIX, TYPE_COLUMN_VAR, TYPE_ROW } dtype;

const char *progname;

ENV *env;
DB *db;

FILE *logfp;
char *logfile;

void	data_set_fix(int, void *, u_int32_t *);
void	data_set_var(int, void *, u_int32_t *, int);
int	dtype_arg(void);
void	key_set(int, void *, u_int32_t *);
int	load(void);
void	progress(const char *, u_int64_t);
int	read_check(void);
void	setup(void);
void	teardown(void);
void	track(const char *, u_int64_t);
void	usage(void);
int	write_check(void);

int
main(int argc, char *argv[])
{
	u_int rand_seed;
	int ch, i, ret, run_cnt;
	int rand_cache, rand_fixed_len, rand_huffman, rand_keys, rand_leaf;
	int rand_node, rand_repeat_compress, rand_type;

	(void)putenv("MALLOC_OPTIONS=AJZ");
	ret = 0;

	if ((progname = strrchr(argv[0], '/')) == NULL)
		progname = argv[0];
	else
		++progname;

	rand_seed = 0xdeadbeef ^ (u_int)time(NULL);
	rand_cache = rand_fixed_len = rand_huffman = rand_keys =
	    rand_leaf = rand_node = rand_repeat_compress = rand_type = 1;
	while ((ch = getopt(argc, argv, "a:Cc:f:h:k:L:l:n:R:r:s:t:v")) != EOF)
		switch (ch) {
		case 'a':
			switch (optarg[0]) {
			case 'd':
				op_dump = 1;
				break;
			case 'r':
				op_reopen = 1;
				break;
			case 'v':
				op_verify = 1;
				break;
			default:
				usage();
				break;
			}
			break;
		case 'C':
			rand_repeat_compress = 0;
			repeat_compress = 1;
			break;
		case 'c':
			rand_cache = 0;
			cachesize = atoi(optarg);
			break;
		case 'f':
			rand_fixed_len = rand_type = 0;
			dtype = TYPE_COLUMN_FIX;
			fixed_len = atoi(optarg);
		case 'h':
			rand_huffman = 0;
			huffman = atoi(optarg);
			break;
		case 'k':
			rand_keys = 0;
			keys = atoi(optarg);
			break;
		case 'L':
			logfile = optarg;
			break;
		case 'l':
			rand_leaf = 0;
			leafsize = atoi(optarg);
			break;
		case 'n':
			rand_node = 0;
			nodesize = atoi(optarg);
			break;
		case 'R':
			rand_seed = (u_int)strtoul(optarg, NULL, 0);
			break;
		case 'r':
			runs = atoi(optarg);
			break;
		case 's':
			switch (optarg[0]) {
			case 'l':
				stat_op = STAT_LOAD;
				break;
			case 'r':
				stat_op = STAT_READ;
				break;
			case 'w':
				stat_op = STAT_WRITE;
				break;
			default:
				stat_op = STAT_ALL;
				break;
			}
			break;
		case 't':
			switch (optarg[0]) {
			case 'r':
				rand_type = 0;
				dtype = TYPE_ROW;
				break;
			case 'v':
				rand_type = 0;
				dtype = TYPE_COLUMN_VAR;
				break;
			default:
				usage();
			}
				
		case 'v':
			verbose = 1;
			break;
		case '?':
		default:
			usage();
		}
	argc -= optind;
	argv += optind;

	if (argc != 0)
		usage();

	printf("t: process %lu\n", (u_long)getpid());
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

		srand(rand_seed);

		/* If no database type, pick one. */
		if (rand_type == 1)
			switch (rand() % 3) {
			case 2:
				dtype = TYPE_COLUMN_FIX;
				break;
			case 1:
				dtype = TYPE_COLUMN_VAR;
				break;
			case 0:
				dtype = TYPE_ROW;
				break;
			}

		/* Data type-specific information. */
		switch (dtype) {
		case TYPE_ROW:
		case TYPE_COLUMN_VAR:
			if (rand_huffman)
				huffman = rand() % 2;
			break;
		case TYPE_COLUMN_FIX:
			if (rand_fixed_len)
				fixed_len = rand() % 34 + 1;
			if (rand_repeat_compress)
				repeat_compress = rand() % 2;
			break;
		}

		/* If no number of keys, choose up to 1M. */
		if (rand_keys)
			keys = 1 + rand() % 999999;

		/* If no cachesize given, choose between 2M and 30M. */
		if (rand_cache)
			cachesize = 2 + rand() % 28;

		/*
		 * If no leafsize or nodesize given, choose between 512B and
		 * 128KB.
		 */
		if (rand_leaf)
			for (leafsize = 512, i = rand() % 9; i > 0; --i)
				leafsize *= 2;
		if (rand_node)
			for (nodesize = 512, i = rand() % 9; i > 0; --i)
				nodesize *= 2;

		printf("%s: %4d { ", progname, run_cnt);
		switch (dtype) {
		case TYPE_COLUMN_FIX:
			printf("-f %d ", fixed_len);
			break;
		case TYPE_COLUMN_VAR:
			printf("-tv ");
			break;
		case TYPE_ROW:
			printf("-tr ");
			break;
		}
		printf("-c %2d -h %d -k %7d -l %6d -n %6d -R %010u }\n\t",
		    cachesize, huffman, keys, leafsize, nodesize, rand_seed);
		(void)fflush(stdout);

		setup();
		if (load() != 0)
			goto err;

#if 1
		if (op_reopen) {
			teardown();
			setup();
		}
		if (dtype == TYPE_ROW && write_check() != 0)
			goto err;

		if (op_reopen) {
			teardown();
			setup();
		}
		switch (dtype) {
		case TYPE_ROW:
			if (read_check_row() != 0)
				goto err;
			break;
		case TYPE_COLUMN_VAR:
			if (read_check_col() != 0)
				goto err;
			break;
		}
#endif
		teardown();

		progress(NULL, 0);
		(void)printf("OK\r");

		rand_seed = rand() ^ time(NULL);
	}

	return (EXIT_SUCCESS);

err:	(void)fprintf(stderr, "\nFAILED!\n");
	return (EXIT_FAILURE);
}

void
setup()
{
	assert(wiredtiger_simple_setup(progname, &db) == 0);
	env = db->env;

	if (logfp != NULL) {
		env->msgfile_set(env, logfp);
		env->verbose_set(env, 0);
	}
	db->errpfx_set(db, progname);
	assert(env->cache_size_set(env, (u_int32_t)cachesize) == 0);
	assert(db->btree_pagesize_set(
	    db, 0, (u_int32_t)nodesize, (u_int32_t)leafsize, 0) == 0);
	switch (dtype) {
	case TYPE_COLUMN_FIX:
		assert(db->column_set(db, fixed_len, NULL,
		    repeat_compress ? WT_REPEAT_COMP : 0) == 0);
		break;
	case TYPE_COLUMN_VAR:
		assert(db->column_set(db, 0, NULL, 0) == 0);
		break;
	case TYPE_ROW:
		break;
	}
	if (huffman)
		assert(db->huffman_set(db, NULL, 0,
		    WT_ASCII_ENGLISH|WT_HUFFMAN_DATA|WT_HUFFMAN_KEY) == 0);
	assert(db->open(db, MYDB, 0660, WT_CREATE) == 0);
}

void
teardown()
{
	assert(wiredtiger_simple_teardown(progname, db) == 0);
}

int
cb_bulk(DB *db, DBT **keyp, DBT **datap)
{
	static DBT key, data;

	if (++keys_cnt == keys + 1)
		return (1);

	if (dtype == TYPE_ROW) {
		key_set(keys_cnt, &key.data, &key.size);
		*keyp = &key;
	} else
		*keyp = NULL;

	if (dtype == TYPE_COLUMN_FIX)
		data_set_fix(keys_cnt, &data.data, &data.size);
	else
		data_set_var(keys_cnt, &data.data, &data.size, 0);
    
	*datap = &data;

	return (0);
}

int
load()
{
	FILE *fp;

	keys_cnt = 0;

	assert(db->bulk_load(db,
	    dtype == TYPE_ROW ? WT_DUPLICATES : 0, track, cb_bulk) == 0);

	if (op_reopen)
		assert(db->sync(db, track, 0) == 0);

	if (op_verify)
		assert(db->verify(db, track, 0) == 0);

	if (op_dump) {
		progress("debug dump", 0);
		assert((fp = fopen(MYDUMP, "w")) != NULL);
		assert(db->dump(db, fp, WT_DEBUG) == 0);
		assert(fclose(fp) == 0);

		progress("print dump", 0);
		assert((fp = fopen(MYPRINT, "w")) != NULL);
		assert(db->dump(db, fp, WT_PRINTABLES) == 0);
		assert(fclose(fp) == 0);
	}

	if (stat_op == STAT_ALL || stat_op == STAT_LOAD) {
		(void)printf("\nLoad statistics:\n");
		assert(env->stat_print(env, stdout, 0) == 0);
	}

	return (0);
}

int
read_check_col()
{
	DBT data;
	WT_TOC *toc;
	u_int64_t cnt, last_cnt;
	u_int32_t dlen;
	char *dbuf;
	int ret;

	memset(&data, 0, sizeof(data));

	assert(env->toc(env, 0, &toc) == 0);

	/* Check a random subset of the records using the record number. */
	for (last_cnt = cnt = 0; cnt < keys;) {
		cnt += rand() % 41 + 1;
		if (cnt > keys)
			cnt = keys;
		if (cnt - last_cnt > 1000) {
			progress("recno read-check", cnt);
			last_cnt = cnt;
		}

		/* Retrieve the key/data pair by record number. */
		if ((ret = db->get_recno(
		    db, toc, (u_int64_t)cnt, NULL, NULL, &data, 0)) != 0) {
			env->err(env,
			    ret, "read_col: get by record failed: %d", cnt);
			assert(0);
		}

		/*
		 * Get local copies of the data, and check to see the retrieved
		 * data is the same.
		 */
		data_set_var(cnt, &dbuf, &dlen, atoi((char *)data.data + 11));
		if (dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			env->errx(env,
			    "read_col: get by record number %d:"
			    "\n\tdata: expected {%s}, got {%.*s}",
			    cnt,
			    dbuf, (int)data.size, (char *)data.data);
			assert(0);
		}
	}

	assert(toc->close(toc, 0) == 0);

	if (stat_op == STAT_ALL || stat_op == STAT_READ) {
		(void)printf("\nRead-check statistics:\n");
		assert(env->stat_print(env, stdout, 0) == 0);
	}

	return (0);
}


int
read_check_row()
{
	DBT key, data;
	WT_TOC *toc;
	u_int64_t cnt, last_cnt;
	u_int32_t klen, dlen;
	char *kbuf, *dbuf;
	int ret;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

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

		/* Retrieve the key/data pair by key. */
		key_set(cnt, &key.data, &key.size);
		if ((ret = db->get(db, toc, &key, NULL, &data, 0)) != 0) {
			env->err(env, ret, "read_row: get by key failed: {%.*s}",
			    (int)key.size, (char *)key.data);
			assert(0);
		}

		/*
		 * Get local copies of the key/data pair, and check to see
		 * if the retrieved key/data pair is the same.
		 */
		key_set(cnt, &kbuf, &klen);
		data_set_var(cnt, &dbuf, &dlen, atoi((char *)data.data + 11));
		if (key.size != klen || memcmp(kbuf, key.data, klen) ||
		    dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			env->errx(env,
			    "read_row: get by key:"
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
		cnt += rand() % 41 + 1;
		if (cnt > keys)
			cnt = keys;
		if (cnt - last_cnt > 1000) {
			progress("recno read-check", cnt);
			last_cnt = cnt;
		}

		/* Retrieve the key/data pair by record number. */
		if ((ret = db->get_recno(
		    db, toc, (u_int64_t)cnt, &key, NULL, &data, 0)) != 0) {
			env->err(env,
			    ret, "read_row: get by record failed: %d", cnt);
			assert(0);
		}

		/*
		 * Get local copies of the key/data pair, and check to see
		 * if the retrieved key/data pair is the same.
		 */
		key_set(cnt, &kbuf, &klen);
		data_set_var(cnt, &dbuf, &dlen, atoi((char *)data.data + 11));
		if (key.size != klen || memcmp(kbuf, key.data, klen) ||
		    dlen != data.size || memcmp(dbuf, data.data, dlen) != 0) {
			env->errx(env,
			    "read_row: get by record number %d:"
			    "\n\tkey: expected {%s}, got {%.*s}; "
			    "\n\tdata: expected {%s}, got {%.*s}",
			    cnt,
			    kbuf, (int)key.size, (char *)key.data,
			    dbuf, (int)data.size, (char *)data.data);
			assert(0);
		}
	}

	assert(toc->close(toc, 0) == 0);

	if (stat_op == STAT_ALL || stat_op == STAT_READ) {
		(void)printf("\nRead-check statistics:\n");
		assert(env->stat_print(env, stdout, 0) == 0);
	}

	return (0);
}

int
write_check()
{
	DBT key, data;
	WT_TOC *toc;
	u_int64_t cnt, last_cnt;
	u_int32_t klen, dlen;
	char *kbuf, *dbuf;
	int ret;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));

	assert(env->toc(env, 0, &toc) == 0);

	/* Update a random subset of the records using the key. */
	for (last_cnt = cnt = 0; cnt < keys;) {
		cnt += rand() % 43 + 1;
		if (cnt > keys)
			cnt = keys;
		if (cnt - last_cnt > 1000) {
			progress("key write-check", cnt);
			last_cnt = cnt;
		}

		/* Retrieve the key/data pair by key. */
		key_set(cnt, &key.data, &key.size);
		if ((ret = db->get(db, toc, &key, NULL, &data, 0)) != 0) {
			env->err(env, ret, "write: get by key failed: {%.*s}",
			    (int)key.size, (char *)key.data);
			assert(0);
		}

		/* Overwrite the key/data pair. */
		if ((ret = db->put(db, toc, &key, &data, 0)) != 0) {
			env->err(env, ret, "write: put by key failed: {%.*s}",
			    (int)key.size, (char *)key.data);
			assert(0);
		}
	}

	assert(toc->close(toc, 0) == 0);

	if (op_verify)
		assert(db->verify(db, track, 0) == 0);

	if (stat_op == STAT_ALL || stat_op == STAT_WRITE) {
		(void)printf("\nWrite-check statistics:\n");
		assert(env->stat_print(env, stdout, 0) == 0);
	}

	return (0);
}

void
key_set(int cnt, void *kbufp, u_int32_t *klenp)
{
	static char kbuf[64];
	int klen;

	/* The key is a 10-digit length. */
	klen = snprintf(kbuf, sizeof(kbuf), "%010d", cnt);
	*(char **)kbufp = kbuf;
	*klenp = (u_int32_t)klen;
}

void
data_set_fix(int cnt, void *dbufp, u_int32_t *dlenp)
{
	static char dbuf[128];
	int ch;

	switch (repeat_compress ? rand() % 2 : cnt % 10) {
	case 9: ch = 'j'; break;
	case 8: ch = 'i'; break;
	case 7: ch = 'h'; break;
	case 6: ch = 'g'; break;
	case 5: ch = 'f'; break;
	case 4: ch = 'e'; break;
	case 3: ch = 'd'; break;
	case 2: ch = 'c'; break;
	case 1: ch = 'b'; break;
	case 0: ch = 'a'; break;
	}
	memset(dbuf, ch, fixed_len);
		
	*(char **)dbufp = dbuf;
	*dlenp = fixed_len;
}

void
data_set_var(int cnt, void *dbufp, u_int32_t *dlenp, int dlen)
{
	static char dbuf[512];

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
track(const char *s, u_int64_t i)
{
	progress(s, i);
}

void
progress(const char *s, u_int64_t i)
{
	static int lastlen = 0;
	int len;
	char *p, msg[128];

	if (!isatty(0))
		return;

	if (s == NULL)
		len = 0;
	else if (i == 0)
		len = snprintf(msg, sizeof(msg), "%s", s);
	else
		len = snprintf(msg, sizeof(msg), "%s %llu", s, i);

	for (p = msg + len; len < lastlen; ++len)
		*p++ = ' ';
	lastlen = len;
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
	    "usage: %s [-Cdv] [-a d|r|v] [-c cachesize] [-f length ] [-h 0|1] "
	    "[-k keys] [-L logfile] [-l leafsize] [-n nodesize] [-R rand] "
	    "[-r runs] [-s l|r|w|*] [-t r|v]\n",
	    progname);
	exit(1);
}
