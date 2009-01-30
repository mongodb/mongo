/* DO NOT EDIT: automatically built by dist/api.py. */

#include "wt_internal.h"

static int __wt_api_db_bulk_load(
	DB *db,
	u_int32_t flags,
	int (*cb)(DB *, DBT **, DBT **));
static int __wt_api_db_bulk_load(
	DB *db,
	u_int32_t flags,
	int (*cb)(DB *, DBT **, DBT **))
{
	wt_args_db_bulk_load args;

	args.db = db;
	args.flags = flags;
	args.cb = cb;
	db->toc->op = WT_OP_DB_BULK_LOAD;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_close(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_close(
	DB *db,
	u_int32_t flags)
{
	wt_args_db_close args;

	args.db = db;
	args.flags = flags;
	db->toc->op = WT_OP_DB_CLOSE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_destroy(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_destroy(
	DB *db,
	u_int32_t flags)
{
	wt_args_db_destroy args;

	args.db = db;
	args.flags = flags;
	db->toc->op = WT_OP_DB_DESTROY;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_dump(
	DB *db,
	FILE *stream,
	u_int32_t flags);
static int __wt_api_db_dump(
	DB *db,
	FILE *stream,
	u_int32_t flags)
{
	wt_args_db_dump args;

	args.db = db;
	args.stream = stream;
	args.flags = flags;
	db->toc->op = WT_OP_DB_DUMP;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_get(
	DB *db,
	DBT *key,
	DBT *pkey,
	DBT *data,
	u_int32_t flags);
static int __wt_api_db_get(
	DB *db,
	DBT *key,
	DBT *pkey,
	DBT *data,
	u_int32_t flags)
{
	wt_args_db_get args;

	args.db = db;
	args.key = key;
	args.pkey = pkey;
	args.data = data;
	args.flags = flags;
	db->toc->op = WT_OP_DB_GET;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static void __wt_api_db_get_btree_compare(
	DB *db,
	int (**btree_compare)(DB *, const DBT *, const DBT *));
static void __wt_api_db_get_btree_compare(
	DB *db,
	int (**btree_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_get_btree_compare args;

	args.db = db;
	args.btree_compare = btree_compare;
	db->toc->op = WT_OP_DB_GET_BTREE_COMPARE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_btree_compare_int(
	DB *db,
	int *btree_compare_int);
static void __wt_api_db_get_btree_compare_int(
	DB *db,
	int *btree_compare_int)
{
	wt_args_db_get_btree_compare_int args;

	args.db = db;
	args.btree_compare_int = btree_compare_int;
	db->toc->op = WT_OP_DB_GET_BTREE_COMPARE_INT;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_btree_dup_compare(
	DB *db,
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *));
static void __wt_api_db_get_btree_dup_compare(
	DB *db,
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_get_btree_dup_compare args;

	args.db = db;
	args.btree_dup_compare = btree_dup_compare;
	db->toc->op = WT_OP_DB_GET_BTREE_DUP_COMPARE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_btree_dup_offpage(
	DB *db,
	u_int32_t *btree_dup_offpage);
static void __wt_api_db_get_btree_dup_offpage(
	DB *db,
	u_int32_t *btree_dup_offpage)
{
	wt_args_db_get_btree_dup_offpage args;

	args.db = db;
	args.btree_dup_offpage = btree_dup_offpage;
	db->toc->op = WT_OP_DB_GET_BTREE_DUP_OFFPAGE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_btree_itemsize(
	DB *db,
	u_int32_t *intlitemsize,
	u_int32_t *leafitemsize);
static void __wt_api_db_get_btree_itemsize(
	DB *db,
	u_int32_t *intlitemsize,
	u_int32_t *leafitemsize)
{
	wt_args_db_get_btree_itemsize args;

	args.db = db;
	args.intlitemsize = intlitemsize;
	args.leafitemsize = leafitemsize;
	db->toc->op = WT_OP_DB_GET_BTREE_ITEMSIZE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_btree_pagesize(
	DB *db,
	u_int32_t *allocsize,
	u_int32_t *intlsize,
	u_int32_t *leafsize,
	u_int32_t *extsize);
static void __wt_api_db_get_btree_pagesize(
	DB *db,
	u_int32_t *allocsize,
	u_int32_t *intlsize,
	u_int32_t *leafsize,
	u_int32_t *extsize)
{
	wt_args_db_get_btree_pagesize args;

	args.db = db;
	args.allocsize = allocsize;
	args.intlsize = intlsize;
	args.leafsize = leafsize;
	args.extsize = extsize;
	db->toc->op = WT_OP_DB_GET_BTREE_PAGESIZE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_errcall(
	DB *db,
	void (**errcall)(const DB *, const char *));
static void __wt_api_db_get_errcall(
	DB *db,
	void (**errcall)(const DB *, const char *))
{
	wt_args_db_get_errcall args;

	args.db = db;
	args.errcall = errcall;
	db->toc->op = WT_OP_DB_GET_ERRCALL;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_errfile(
	DB *db,
	FILE **errfile);
static void __wt_api_db_get_errfile(
	DB *db,
	FILE **errfile)
{
	wt_args_db_get_errfile args;

	args.db = db;
	args.errfile = errfile;
	db->toc->op = WT_OP_DB_GET_ERRFILE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static void __wt_api_db_get_errpfx(
	DB *db,
	const char **errpfx);
static void __wt_api_db_get_errpfx(
	DB *db,
	const char **errpfx)
{
	wt_args_db_get_errpfx args;

	args.db = db;
	args.errpfx = errpfx;
	db->toc->op = WT_OP_DB_GET_ERRPFX;
	db->toc->argp = &args;
	db->toc->env = db->env;

	(void)__wt_toc_sched(db->toc);
}

static int __wt_api_db_open(
	DB *db,
	const char *dbname,
	mode_t mode,
	u_int32_t flags);
static int __wt_api_db_open(
	DB *db,
	const char *dbname,
	mode_t mode,
	u_int32_t flags)
{
	wt_args_db_open args;

	args.db = db;
	args.dbname = dbname;
	args.mode = mode;
	args.flags = flags;
	db->toc->op = WT_OP_DB_OPEN;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_btree_compare(
	DB *db,
	int (*btree_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_set_btree_compare(
	DB *db,
	int (*btree_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_set_btree_compare args;

	args.db = db;
	args.btree_compare = btree_compare;
	db->toc->op = WT_OP_DB_SET_BTREE_COMPARE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_btree_compare_int(
	DB *db,
	int btree_compare_int);
static int __wt_api_db_set_btree_compare_int(
	DB *db,
	int btree_compare_int)
{
	wt_args_db_set_btree_compare_int args;

	args.db = db;
	args.btree_compare_int = btree_compare_int;
	db->toc->op = WT_OP_DB_SET_BTREE_COMPARE_INT;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_btree_dup_compare(
	DB *db,
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_set_btree_dup_compare(
	DB *db,
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_set_btree_dup_compare args;

	args.db = db;
	args.btree_dup_compare = btree_dup_compare;
	db->toc->op = WT_OP_DB_SET_BTREE_DUP_COMPARE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_btree_dup_offpage(
	DB *db,
	u_int32_t btree_dup_offpage);
static int __wt_api_db_set_btree_dup_offpage(
	DB *db,
	u_int32_t btree_dup_offpage)
{
	wt_args_db_set_btree_dup_offpage args;

	args.db = db;
	args.btree_dup_offpage = btree_dup_offpage;
	db->toc->op = WT_OP_DB_SET_BTREE_DUP_OFFPAGE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_btree_itemsize(
	DB *db,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize);
static int __wt_api_db_set_btree_itemsize(
	DB *db,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize)
{
	wt_args_db_set_btree_itemsize args;

	args.db = db;
	args.intlitemsize = intlitemsize;
	args.leafitemsize = leafitemsize;
	db->toc->op = WT_OP_DB_SET_BTREE_ITEMSIZE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_btree_pagesize(
	DB *db,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize);
static int __wt_api_db_set_btree_pagesize(
	DB *db,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize)
{
	wt_args_db_set_btree_pagesize args;

	args.db = db;
	args.allocsize = allocsize;
	args.intlsize = intlsize;
	args.leafsize = leafsize;
	args.extsize = extsize;
	db->toc->op = WT_OP_DB_SET_BTREE_PAGESIZE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_errcall(
	DB *db,
	void (*errcall)(const DB *, const char *));
static int __wt_api_db_set_errcall(
	DB *db,
	void (*errcall)(const DB *, const char *))
{
	wt_args_db_set_errcall args;

	args.db = db;
	args.errcall = errcall;
	db->toc->op = WT_OP_DB_SET_ERRCALL;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_errfile(
	DB *db,
	FILE *errfile);
static int __wt_api_db_set_errfile(
	DB *db,
	FILE *errfile)
{
	wt_args_db_set_errfile args;

	args.db = db;
	args.errfile = errfile;
	db->toc->op = WT_OP_DB_SET_ERRFILE;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_set_errpfx(
	DB *db,
	const char *errpfx);
static int __wt_api_db_set_errpfx(
	DB *db,
	const char *errpfx)
{
	wt_args_db_set_errpfx args;

	args.db = db;
	args.errpfx = errpfx;
	db->toc->op = WT_OP_DB_SET_ERRPFX;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_stat_clear(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_stat_clear(
	DB *db,
	u_int32_t flags)
{
	wt_args_db_stat_clear args;

	args.db = db;
	args.flags = flags;
	db->toc->op = WT_OP_DB_STAT_CLEAR;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_stat_print(
	DB *db,
	FILE * stream,
	u_int32_t flags);
static int __wt_api_db_stat_print(
	DB *db,
	FILE * stream,
	u_int32_t flags)
{
	wt_args_db_stat_print args;

	args.db = db;
	args.stream = stream;
	args.flags = flags;
	db->toc->op = WT_OP_DB_STAT_PRINT;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_sync(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_sync(
	DB *db,
	u_int32_t flags)
{
	wt_args_db_sync args;

	args.db = db;
	args.flags = flags;
	db->toc->op = WT_OP_DB_SYNC;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_db_verify(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_verify(
	DB *db,
	u_int32_t flags)
{
	wt_args_db_verify args;

	args.db = db;
	args.flags = flags;
	db->toc->op = WT_OP_DB_VERIFY;
	db->toc->argp = &args;
	db->toc->env = db->env;

	return (__wt_toc_sched(db->toc));
}

static int __wt_api_env_close(
	ENV *env,
	u_int32_t flags);
static int __wt_api_env_close(
	ENV *env,
	u_int32_t flags)
{
	wt_args_env_close args;

	args.env = env;
	args.flags = flags;
	env->toc->op = WT_OP_ENV_CLOSE;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_destroy(
	ENV *env,
	u_int32_t flags);
static int __wt_api_env_destroy(
	ENV *env,
	u_int32_t flags)
{
	wt_args_env_destroy args;

	args.env = env;
	args.flags = flags;
	env->toc->op = WT_OP_ENV_DESTROY;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static void __wt_api_env_get_cachesize(
	ENV *env,
	u_int32_t *cachesize);
static void __wt_api_env_get_cachesize(
	ENV *env,
	u_int32_t *cachesize)
{
	wt_args_env_get_cachesize args;

	args.env = env;
	args.cachesize = cachesize;
	env->toc->op = WT_OP_ENV_GET_CACHESIZE;
	env->toc->argp = &args;
	env->toc->env = env;

	(void)__wt_toc_sched(env->toc);
}

static void __wt_api_env_get_errcall(
	ENV *env,
	void (**errcall)(const ENV *, const char *));
static void __wt_api_env_get_errcall(
	ENV *env,
	void (**errcall)(const ENV *, const char *))
{
	wt_args_env_get_errcall args;

	args.env = env;
	args.errcall = errcall;
	env->toc->op = WT_OP_ENV_GET_ERRCALL;
	env->toc->argp = &args;
	env->toc->env = env;

	(void)__wt_toc_sched(env->toc);
}

static void __wt_api_env_get_errfile(
	ENV *env,
	FILE **errfile);
static void __wt_api_env_get_errfile(
	ENV *env,
	FILE **errfile)
{
	wt_args_env_get_errfile args;

	args.env = env;
	args.errfile = errfile;
	env->toc->op = WT_OP_ENV_GET_ERRFILE;
	env->toc->argp = &args;
	env->toc->env = env;

	(void)__wt_toc_sched(env->toc);
}

static void __wt_api_env_get_errpfx(
	ENV *env,
	const char **errpfx);
static void __wt_api_env_get_errpfx(
	ENV *env,
	const char **errpfx)
{
	wt_args_env_get_errpfx args;

	args.env = env;
	args.errpfx = errpfx;
	env->toc->op = WT_OP_ENV_GET_ERRPFX;
	env->toc->argp = &args;
	env->toc->env = env;

	(void)__wt_toc_sched(env->toc);
}

static void __wt_api_env_get_verbose(
	ENV *env,
	u_int32_t *verbose);
static void __wt_api_env_get_verbose(
	ENV *env,
	u_int32_t *verbose)
{
	wt_args_env_get_verbose args;

	args.env = env;
	args.verbose = verbose;
	env->toc->op = WT_OP_ENV_GET_VERBOSE;
	env->toc->argp = &args;
	env->toc->env = env;

	(void)__wt_toc_sched(env->toc);
}

static int __wt_api_env_open(
	ENV *env,
	const char *home,
	mode_t mode,
	u_int32_t flags);
static int __wt_api_env_open(
	ENV *env,
	const char *home,
	mode_t mode,
	u_int32_t flags)
{
	wt_args_env_open args;

	args.env = env;
	args.home = home;
	args.mode = mode;
	args.flags = flags;
	env->toc->op = WT_OP_ENV_OPEN;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_set_cachesize(
	ENV *env,
	u_int32_t cachesize);
static int __wt_api_env_set_cachesize(
	ENV *env,
	u_int32_t cachesize)
{
	wt_args_env_set_cachesize args;

	args.env = env;
	args.cachesize = cachesize;
	env->toc->op = WT_OP_ENV_SET_CACHESIZE;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_set_errcall(
	ENV *env,
	void (*errcall)(const ENV *, const char *));
static int __wt_api_env_set_errcall(
	ENV *env,
	void (*errcall)(const ENV *, const char *))
{
	wt_args_env_set_errcall args;

	args.env = env;
	args.errcall = errcall;
	env->toc->op = WT_OP_ENV_SET_ERRCALL;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_set_errfile(
	ENV *env,
	FILE *errfile);
static int __wt_api_env_set_errfile(
	ENV *env,
	FILE *errfile)
{
	wt_args_env_set_errfile args;

	args.env = env;
	args.errfile = errfile;
	env->toc->op = WT_OP_ENV_SET_ERRFILE;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_set_errpfx(
	ENV *env,
	const char *errpfx);
static int __wt_api_env_set_errpfx(
	ENV *env,
	const char *errpfx)
{
	wt_args_env_set_errpfx args;

	args.env = env;
	args.errpfx = errpfx;
	env->toc->op = WT_OP_ENV_SET_ERRPFX;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_set_verbose(
	ENV *env,
	u_int32_t verbose);
static int __wt_api_env_set_verbose(
	ENV *env,
	u_int32_t verbose)
{
	wt_args_env_set_verbose args;

	args.env = env;
	args.verbose = verbose;
	env->toc->op = WT_OP_ENV_SET_VERBOSE;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_stat_clear(
	ENV *env,
	u_int32_t flags);
static int __wt_api_env_stat_clear(
	ENV *env,
	u_int32_t flags)
{
	wt_args_env_stat_clear args;

	args.env = env;
	args.flags = flags;
	env->toc->op = WT_OP_ENV_STAT_CLEAR;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static int __wt_api_env_stat_print(
	ENV *env,
	FILE *stream,
	u_int32_t flags);
static int __wt_api_env_stat_print(
	ENV *env,
	FILE *stream,
	u_int32_t flags)
{
	wt_args_env_stat_print args;

	args.env = env;
	args.stream = stream;
	args.flags = flags;
	env->toc->op = WT_OP_ENV_STAT_PRINT;
	env->toc->argp = &args;
	env->toc->env = env;

	return (__wt_toc_sched(env->toc));
}

static void __wt_db_get_btree_compare(
    wt_args_db_get_btree_compare *argp);
static void __wt_db_get_btree_compare(
    wt_args_db_get_btree_compare *argp)
{
	*(argp->btree_compare) = argp->db->btree_compare;
}

static void __wt_db_get_btree_compare_int(
    wt_args_db_get_btree_compare_int *argp);
static void __wt_db_get_btree_compare_int(
    wt_args_db_get_btree_compare_int *argp)
{
	*(argp->btree_compare_int) = argp->db->btree_compare_int;
}

static void __wt_db_get_btree_dup_compare(
    wt_args_db_get_btree_dup_compare *argp);
static void __wt_db_get_btree_dup_compare(
    wt_args_db_get_btree_dup_compare *argp)
{
	*(argp->btree_dup_compare) = argp->db->btree_dup_compare;
}

static void __wt_db_get_btree_dup_offpage(
    wt_args_db_get_btree_dup_offpage *argp);
static void __wt_db_get_btree_dup_offpage(
    wt_args_db_get_btree_dup_offpage *argp)
{
	*(argp->btree_dup_offpage) = argp->db->btree_dup_offpage;
}

static void __wt_db_get_btree_itemsize(
    wt_args_db_get_btree_itemsize *argp);
static void __wt_db_get_btree_itemsize(
    wt_args_db_get_btree_itemsize *argp)
{
	*(argp->intlitemsize) = argp->db->intlitemsize;
	*(argp->leafitemsize) = argp->db->leafitemsize;
}

static void __wt_db_get_btree_pagesize(
    wt_args_db_get_btree_pagesize *argp);
static void __wt_db_get_btree_pagesize(
    wt_args_db_get_btree_pagesize *argp)
{
	*(argp->allocsize) = argp->db->allocsize;
	*(argp->intlsize) = argp->db->intlsize;
	*(argp->leafsize) = argp->db->leafsize;
	*(argp->extsize) = argp->db->extsize;
}

static void __wt_db_get_errcall(
    wt_args_db_get_errcall *argp);
static void __wt_db_get_errcall(
    wt_args_db_get_errcall *argp)
{
	*(argp->errcall) = argp->db->errcall;
}

static void __wt_db_get_errfile(
    wt_args_db_get_errfile *argp);
static void __wt_db_get_errfile(
    wt_args_db_get_errfile *argp)
{
	*(argp->errfile) = argp->db->errfile;
}

static void __wt_db_get_errpfx(
    wt_args_db_get_errpfx *argp);
static void __wt_db_get_errpfx(
    wt_args_db_get_errpfx *argp)
{
	*(argp->errpfx) = argp->db->errpfx;
}

static int __wt_db_set_btree_compare(
    wt_args_db_set_btree_compare *argp);
static int __wt_db_set_btree_compare(
    wt_args_db_set_btree_compare *argp)
{
	argp->db->btree_compare = argp->btree_compare;
	return (0);
}

static int __wt_db_set_btree_compare_int(
    wt_args_db_set_btree_compare_int *argp);
static int __wt_db_set_btree_compare_int(
    wt_args_db_set_btree_compare_int *argp)
{
	int ret;

	if ((ret = __wt_db_set_btree_compare_int_verify(argp)) != 0)
		return (ret);

	argp->db->btree_compare_int = argp->btree_compare_int;
	return (0);
}

static int __wt_db_set_btree_dup_compare(
    wt_args_db_set_btree_dup_compare *argp);
static int __wt_db_set_btree_dup_compare(
    wt_args_db_set_btree_dup_compare *argp)
{
	argp->db->btree_dup_compare = argp->btree_dup_compare;
	return (0);
}

static int __wt_db_set_btree_dup_offpage(
    wt_args_db_set_btree_dup_offpage *argp);
static int __wt_db_set_btree_dup_offpage(
    wt_args_db_set_btree_dup_offpage *argp)
{
	argp->db->btree_dup_offpage = argp->btree_dup_offpage;
	return (0);
}

static int __wt_db_set_btree_itemsize(
    wt_args_db_set_btree_itemsize *argp);
static int __wt_db_set_btree_itemsize(
    wt_args_db_set_btree_itemsize *argp)
{
	argp->db->intlitemsize = argp->intlitemsize;
	argp->db->leafitemsize = argp->leafitemsize;
	return (0);
}

static int __wt_db_set_btree_pagesize(
    wt_args_db_set_btree_pagesize *argp);
static int __wt_db_set_btree_pagesize(
    wt_args_db_set_btree_pagesize *argp)
{
	argp->db->allocsize = argp->allocsize;
	argp->db->intlsize = argp->intlsize;
	argp->db->leafsize = argp->leafsize;
	argp->db->extsize = argp->extsize;
	return (0);
}

static int __wt_db_set_errcall(
    wt_args_db_set_errcall *argp);
static int __wt_db_set_errcall(
    wt_args_db_set_errcall *argp)
{
	argp->db->errcall = argp->errcall;
	return (0);
}

static int __wt_db_set_errfile(
    wt_args_db_set_errfile *argp);
static int __wt_db_set_errfile(
    wt_args_db_set_errfile *argp)
{
	argp->db->errfile = argp->errfile;
	return (0);
}

static int __wt_db_set_errpfx(
    wt_args_db_set_errpfx *argp);
static int __wt_db_set_errpfx(
    wt_args_db_set_errpfx *argp)
{
	argp->db->errpfx = argp->errpfx;
	return (0);
}

static void __wt_env_get_cachesize(
    wt_args_env_get_cachesize *argp);
static void __wt_env_get_cachesize(
    wt_args_env_get_cachesize *argp)
{
	*(argp->cachesize) = argp->env->cachesize;
}

static void __wt_env_get_errcall(
    wt_args_env_get_errcall *argp);
static void __wt_env_get_errcall(
    wt_args_env_get_errcall *argp)
{
	*(argp->errcall) = argp->env->errcall;
}

static void __wt_env_get_errfile(
    wt_args_env_get_errfile *argp);
static void __wt_env_get_errfile(
    wt_args_env_get_errfile *argp)
{
	*(argp->errfile) = argp->env->errfile;
}

static void __wt_env_get_errpfx(
    wt_args_env_get_errpfx *argp);
static void __wt_env_get_errpfx(
    wt_args_env_get_errpfx *argp)
{
	*(argp->errpfx) = argp->env->errpfx;
}

static void __wt_env_get_verbose(
    wt_args_env_get_verbose *argp);
static void __wt_env_get_verbose(
    wt_args_env_get_verbose *argp)
{
	*(argp->verbose) = argp->env->verbose;
}

static int __wt_env_set_cachesize(
    wt_args_env_set_cachesize *argp);
static int __wt_env_set_cachesize(
    wt_args_env_set_cachesize *argp)
{
	argp->env->cachesize = argp->cachesize;
	return (0);
}

static int __wt_env_set_errcall(
    wt_args_env_set_errcall *argp);
static int __wt_env_set_errcall(
    wt_args_env_set_errcall *argp)
{
	argp->env->errcall = argp->errcall;
	return (0);
}

static int __wt_env_set_errfile(
    wt_args_env_set_errfile *argp);
static int __wt_env_set_errfile(
    wt_args_env_set_errfile *argp)
{
	argp->env->errfile = argp->errfile;
	return (0);
}

static int __wt_env_set_errpfx(
    wt_args_env_set_errpfx *argp);
static int __wt_env_set_errpfx(
    wt_args_env_set_errpfx *argp)
{
	argp->env->errpfx = argp->errpfx;
	return (0);
}

static int __wt_env_set_verbose(
    wt_args_env_set_verbose *argp);
static int __wt_env_set_verbose(
    wt_args_env_set_verbose *argp)
{
	int ret;

	if ((ret = __wt_env_set_verbose_verify(argp)) != 0)
		return (ret);

	argp->env->verbose = argp->verbose;
	return (0);
}

void
__wt_env_config_methods(ENV *env)
{
	env->close = __wt_api_env_close;
	env->destroy = __wt_api_env_destroy;
	env->err = __wt_env_err;
	env->errx = __wt_env_errx;
	env->get_cachesize = __wt_api_env_get_cachesize;
	env->get_errcall = __wt_api_env_get_errcall;
	env->get_errfile = __wt_api_env_get_errfile;
	env->get_errpfx = __wt_api_env_get_errpfx;
	env->get_verbose = __wt_api_env_get_verbose;
	env->open = __wt_api_env_open;
	env->set_cachesize = __wt_api_env_set_cachesize;
	env->set_errcall = __wt_api_env_set_errcall;
	env->set_errfile = __wt_api_env_set_errfile;
	env->set_errpfx = __wt_api_env_set_errpfx;
	env->set_verbose = __wt_api_env_set_verbose;
	env->stat_clear = __wt_api_env_stat_clear;
	env->stat_print = __wt_api_env_stat_print;
}

void
__wt_env_config_methods_open(ENV *env)
{
}

void
__wt_env_config_methods_lockout(ENV *env)
{
	env->close = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout_err;
	env->err = (void (*)
	    (ENV *, int , const char *, ...))
	    __wt_env_lockout_err;
	env->errx = (void (*)
	    (ENV *, const char *, ...))
	    __wt_env_lockout_err;
	env->get_cachesize = (void (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout_err;
	env->get_errcall = (void (*)
	    (ENV *, void (**)(const ENV *, const char *)))
	    __wt_env_lockout_err;
	env->get_errfile = (void (*)
	    (ENV *, FILE **))
	    __wt_env_lockout_err;
	env->get_errpfx = (void (*)
	    (ENV *, const char **))
	    __wt_env_lockout_err;
	env->get_verbose = (void (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout_err;
	env->open = (int (*)
	    (ENV *, const char *, mode_t , u_int32_t ))
	    __wt_env_lockout_err;
	env->set_cachesize = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout_err;
	env->set_errcall = (int (*)
	    (ENV *, void (*)(const ENV *, const char *)))
	    __wt_env_lockout_err;
	env->set_errfile = (int (*)
	    (ENV *, FILE *))
	    __wt_env_lockout_err;
	env->set_errpfx = (int (*)
	    (ENV *, const char *))
	    __wt_env_lockout_err;
	env->set_verbose = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout_err;
	env->stat_clear = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout_err;
	env->stat_print = (int (*)
	    (ENV *, FILE *, u_int32_t ))
	    __wt_env_lockout_err;
}

void
__wt_db_config_methods(DB *db)
{
	db->bulk_load = __wt_api_db_bulk_load;
	db->close = __wt_api_db_close;
	db->destroy = __wt_api_db_destroy;
	db->dump = (int (*)
	    (DB *, FILE *, u_int32_t ))
	    __wt_db_lockout_open;
	db->err = __wt_db_err;
	db->errx = __wt_db_errx;
	db->get = (int (*)
	    (DB *, DBT *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout_open;
	db->get_btree_compare = __wt_api_db_get_btree_compare;
	db->get_btree_compare_int = __wt_api_db_get_btree_compare_int;
	db->get_btree_dup_compare = __wt_api_db_get_btree_dup_compare;
	db->get_btree_dup_offpage = __wt_api_db_get_btree_dup_offpage;
	db->get_btree_itemsize = __wt_api_db_get_btree_itemsize;
	db->get_btree_pagesize = __wt_api_db_get_btree_pagesize;
	db->get_errcall = __wt_api_db_get_errcall;
	db->get_errfile = __wt_api_db_get_errfile;
	db->get_errpfx = __wt_api_db_get_errpfx;
	db->open = __wt_api_db_open;
	db->set_btree_compare = __wt_api_db_set_btree_compare;
	db->set_btree_compare_int = __wt_api_db_set_btree_compare_int;
	db->set_btree_dup_compare = __wt_api_db_set_btree_dup_compare;
	db->set_btree_dup_offpage = __wt_api_db_set_btree_dup_offpage;
	db->set_btree_itemsize = __wt_api_db_set_btree_itemsize;
	db->set_btree_pagesize = __wt_api_db_set_btree_pagesize;
	db->set_errcall = __wt_api_db_set_errcall;
	db->set_errfile = __wt_api_db_set_errfile;
	db->set_errpfx = __wt_api_db_set_errpfx;
	db->stat_clear = __wt_api_db_stat_clear;
	db->stat_print = __wt_api_db_stat_print;
	db->sync = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_open;
	db->verify = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_open;
}

void
__wt_db_config_methods_open(DB *db)
{
	db->dump = __wt_api_db_dump;
	db->get = __wt_api_db_get;
	db->sync = __wt_api_db_sync;
	db->verify = __wt_api_db_verify;
}
void
__wt_db_config_methods_lockout(DB *db)
{
	db->bulk_load = (int (*)
	    (DB *, u_int32_t , int (*)(DB *, DBT **, DBT **)))
	    __wt_db_lockout_err;
	db->close = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_err;
	db->dump = (int (*)
	    (DB *, FILE *, u_int32_t ))
	    __wt_db_lockout_err;
	db->err = (void (*)
	    (DB *, int , const char *, ...))
	    __wt_db_lockout_err;
	db->errx = (void (*)
	    (DB *, const char *, ...))
	    __wt_db_lockout_err;
	db->get = (int (*)
	    (DB *, DBT *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout_err;
	db->get_btree_compare = (void (*)
	    (DB *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->get_btree_compare_int = (void (*)
	    (DB *, int *))
	    __wt_db_lockout_err;
	db->get_btree_dup_compare = (void (*)
	    (DB *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->get_btree_dup_offpage = (void (*)
	    (DB *, u_int32_t *))
	    __wt_db_lockout_err;
	db->get_btree_itemsize = (void (*)
	    (DB *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout_err;
	db->get_btree_pagesize = (void (*)
	    (DB *, u_int32_t *, u_int32_t *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout_err;
	db->get_errcall = (void (*)
	    (DB *, void (**)(const DB *, const char *)))
	    __wt_db_lockout_err;
	db->get_errfile = (void (*)
	    (DB *, FILE **))
	    __wt_db_lockout_err;
	db->get_errpfx = (void (*)
	    (DB *, const char **))
	    __wt_db_lockout_err;
	db->open = (int (*)
	    (DB *, const char *, mode_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->set_btree_compare = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->set_btree_compare_int = (int (*)
	    (DB *, int ))
	    __wt_db_lockout_err;
	db->set_btree_dup_compare = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->set_btree_dup_offpage = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_err;
	db->set_btree_itemsize = (int (*)
	    (DB *, u_int32_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->set_btree_pagesize = (int (*)
	    (DB *, u_int32_t , u_int32_t , u_int32_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->set_errcall = (int (*)
	    (DB *, void (*)(const DB *, const char *)))
	    __wt_db_lockout_err;
	db->set_errfile = (int (*)
	    (DB *, FILE *))
	    __wt_db_lockout_err;
	db->set_errpfx = (int (*)
	    (DB *, const char *))
	    __wt_db_lockout_err;
	db->stat_clear = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_err;
	db->stat_print = (int (*)
	    (DB *, FILE * , u_int32_t ))
	    __wt_db_lockout_err;
	db->sync = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_err;
	db->verify = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_err;
}

int
__wt_api_switch(WT_TOC *toc)
{
	void *argp;
	int ret;

	argp = toc->argp;
	ret = 0;

	switch (toc->op) {
	case WT_OP_DB_BULK_LOAD:
		ret = __wt_db_bulk_load(argp);
		break;
	case WT_OP_DB_CLOSE:
		ret = __wt_db_close(argp);
		break;
	case WT_OP_DB_DESTROY:
		ret = __wt_db_destroy(argp);
		break;
	case WT_OP_DB_DUMP:
		ret = __wt_db_dump(argp);
		break;
	case WT_OP_DB_GET:
		ret = __wt_db_get(argp);
		break;
	case WT_OP_DB_GET_BTREE_COMPARE:
		__wt_db_get_btree_compare(argp);
		break;
	case WT_OP_DB_GET_BTREE_COMPARE_INT:
		__wt_db_get_btree_compare_int(argp);
		break;
	case WT_OP_DB_GET_BTREE_DUP_COMPARE:
		__wt_db_get_btree_dup_compare(argp);
		break;
	case WT_OP_DB_GET_BTREE_DUP_OFFPAGE:
		__wt_db_get_btree_dup_offpage(argp);
		break;
	case WT_OP_DB_GET_BTREE_ITEMSIZE:
		__wt_db_get_btree_itemsize(argp);
		break;
	case WT_OP_DB_GET_BTREE_PAGESIZE:
		__wt_db_get_btree_pagesize(argp);
		break;
	case WT_OP_DB_GET_ERRCALL:
		__wt_db_get_errcall(argp);
		break;
	case WT_OP_DB_GET_ERRFILE:
		__wt_db_get_errfile(argp);
		break;
	case WT_OP_DB_GET_ERRPFX:
		__wt_db_get_errpfx(argp);
		break;
	case WT_OP_DB_OPEN:
		ret = __wt_db_open(argp);
		break;
	case WT_OP_DB_SET_BTREE_COMPARE:
		ret = __wt_db_set_btree_compare(argp);
		break;
	case WT_OP_DB_SET_BTREE_COMPARE_INT:
		ret = __wt_db_set_btree_compare_int(argp);
		break;
	case WT_OP_DB_SET_BTREE_DUP_COMPARE:
		ret = __wt_db_set_btree_dup_compare(argp);
		break;
	case WT_OP_DB_SET_BTREE_DUP_OFFPAGE:
		ret = __wt_db_set_btree_dup_offpage(argp);
		break;
	case WT_OP_DB_SET_BTREE_ITEMSIZE:
		ret = __wt_db_set_btree_itemsize(argp);
		break;
	case WT_OP_DB_SET_BTREE_PAGESIZE:
		ret = __wt_db_set_btree_pagesize(argp);
		break;
	case WT_OP_DB_SET_ERRCALL:
		ret = __wt_db_set_errcall(argp);
		break;
	case WT_OP_DB_SET_ERRFILE:
		ret = __wt_db_set_errfile(argp);
		break;
	case WT_OP_DB_SET_ERRPFX:
		ret = __wt_db_set_errpfx(argp);
		break;
	case WT_OP_DB_STAT_CLEAR:
		ret = __wt_db_stat_clear(argp);
		break;
	case WT_OP_DB_STAT_PRINT:
		ret = __wt_db_stat_print(argp);
		break;
	case WT_OP_DB_SYNC:
		ret = __wt_db_sync(argp);
		break;
	case WT_OP_DB_VERIFY:
		ret = __wt_db_verify(argp);
		break;
	case WT_OP_ENV_CLOSE:
		ret = __wt_env_close(argp);
		break;
	case WT_OP_ENV_DESTROY:
		ret = __wt_env_destroy(argp);
		break;
	case WT_OP_ENV_GET_CACHESIZE:
		__wt_env_get_cachesize(argp);
		break;
	case WT_OP_ENV_GET_ERRCALL:
		__wt_env_get_errcall(argp);
		break;
	case WT_OP_ENV_GET_ERRFILE:
		__wt_env_get_errfile(argp);
		break;
	case WT_OP_ENV_GET_ERRPFX:
		__wt_env_get_errpfx(argp);
		break;
	case WT_OP_ENV_GET_VERBOSE:
		__wt_env_get_verbose(argp);
		break;
	case WT_OP_ENV_OPEN:
		ret = __wt_env_open(argp);
		break;
	case WT_OP_ENV_SET_CACHESIZE:
		ret = __wt_env_set_cachesize(argp);
		break;
	case WT_OP_ENV_SET_ERRCALL:
		ret = __wt_env_set_errcall(argp);
		break;
	case WT_OP_ENV_SET_ERRFILE:
		ret = __wt_env_set_errfile(argp);
		break;
	case WT_OP_ENV_SET_ERRPFX:
		ret = __wt_env_set_errpfx(argp);
		break;
	case WT_OP_ENV_SET_VERBOSE:
		ret = __wt_env_set_verbose(argp);
		break;
	case WT_OP_ENV_STAT_CLEAR:
		ret = __wt_env_stat_clear(argp);
		break;
	case WT_OP_ENV_STAT_PRINT:
		ret = __wt_env_stat_print(argp);
		break;
	default:
		ret = WT_ERROR;
		break;
	}

	return (ret);
}
