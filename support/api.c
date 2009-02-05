/* DO NOT EDIT: automatically built by dist/api.py. */

#include "wt_internal.h"

static int __wt_api_db_bulk_load(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags,
	int (*cb)(DB *, DBT **, DBT **));
static int __wt_api_db_bulk_load(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags,
	int (*cb)(DB *, DBT **, DBT **))
{
	wt_args_db_bulk_load args;

	wt_args_db_bulk_load_pack;

	wt_args_db_toc_sched(WT_OP_DB_BULK_LOAD);
}

static int __wt_api_db_close(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_db_close(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_db_close args;

	wt_args_db_close_pack;

	wt_args_db_toc_sched(WT_OP_DB_CLOSE);
}

static int __wt_api_db_destroy(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_db_destroy(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_db_destroy args;

	wt_args_db_destroy_pack;

	wt_args_db_toc_sched(WT_OP_DB_DESTROY);
}

static int __wt_api_db_dump(
	DB *db,
	WT_TOC *toc,
	FILE *stream,
	u_int32_t flags);
static int __wt_api_db_dump(
	DB *db,
	WT_TOC *toc,
	FILE *stream,
	u_int32_t flags)
{
	wt_args_db_dump args;

	wt_args_db_dump_pack;

	wt_args_db_toc_sched(WT_OP_DB_DUMP);
}

static int __wt_api_db_get(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *pkey,
	DBT *data,
	u_int32_t flags);
static int __wt_api_db_get(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *pkey,
	DBT *data,
	u_int32_t flags)
{
	wt_args_db_get args;

	wt_args_db_get_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET);
}

static int __wt_api_db_get_btree_compare(
	DB *db,
	WT_TOC *toc,
	int (**btree_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_get_btree_compare(
	DB *db,
	WT_TOC *toc,
	int (**btree_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_get_btree_compare args;

	wt_args_db_get_btree_compare_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_BTREE_COMPARE);
}

static int __wt_api_db_get_btree_compare_int(
	DB *db,
	WT_TOC *toc,
	int *btree_compare_int);
static int __wt_api_db_get_btree_compare_int(
	DB *db,
	WT_TOC *toc,
	int *btree_compare_int)
{
	wt_args_db_get_btree_compare_int args;

	wt_args_db_get_btree_compare_int_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_BTREE_COMPARE_INT);
}

static int __wt_api_db_get_btree_dup_compare(
	DB *db,
	WT_TOC *toc,
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_get_btree_dup_compare(
	DB *db,
	WT_TOC *toc,
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_get_btree_dup_compare args;

	wt_args_db_get_btree_dup_compare_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_BTREE_DUP_COMPARE);
}

static int __wt_api_db_get_btree_dup_offpage(
	DB *db,
	WT_TOC *toc,
	u_int32_t *btree_dup_offpage);
static int __wt_api_db_get_btree_dup_offpage(
	DB *db,
	WT_TOC *toc,
	u_int32_t *btree_dup_offpage)
{
	wt_args_db_get_btree_dup_offpage args;

	wt_args_db_get_btree_dup_offpage_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_BTREE_DUP_OFFPAGE);
}

static int __wt_api_db_get_btree_itemsize(
	DB *db,
	WT_TOC *toc,
	u_int32_t *intlitemsize,
	u_int32_t *leafitemsize);
static int __wt_api_db_get_btree_itemsize(
	DB *db,
	WT_TOC *toc,
	u_int32_t *intlitemsize,
	u_int32_t *leafitemsize)
{
	wt_args_db_get_btree_itemsize args;

	wt_args_db_get_btree_itemsize_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_BTREE_ITEMSIZE);
}

static int __wt_api_db_get_btree_pagesize(
	DB *db,
	WT_TOC *toc,
	u_int32_t *allocsize,
	u_int32_t *intlsize,
	u_int32_t *leafsize,
	u_int32_t *extsize);
static int __wt_api_db_get_btree_pagesize(
	DB *db,
	WT_TOC *toc,
	u_int32_t *allocsize,
	u_int32_t *intlsize,
	u_int32_t *leafsize,
	u_int32_t *extsize)
{
	wt_args_db_get_btree_pagesize args;

	wt_args_db_get_btree_pagesize_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_BTREE_PAGESIZE);
}

static int __wt_api_db_get_errcall(
	DB *db,
	WT_TOC *toc,
	void (**errcall)(const DB *, const char *));
static int __wt_api_db_get_errcall(
	DB *db,
	WT_TOC *toc,
	void (**errcall)(const DB *, const char *))
{
	wt_args_db_get_errcall args;

	wt_args_db_get_errcall_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_ERRCALL);
}

static int __wt_api_db_get_errfile(
	DB *db,
	WT_TOC *toc,
	FILE **errfile);
static int __wt_api_db_get_errfile(
	DB *db,
	WT_TOC *toc,
	FILE **errfile)
{
	wt_args_db_get_errfile args;

	wt_args_db_get_errfile_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_ERRFILE);
}

static int __wt_api_db_get_errpfx(
	DB *db,
	WT_TOC *toc,
	const char **errpfx);
static int __wt_api_db_get_errpfx(
	DB *db,
	WT_TOC *toc,
	const char **errpfx)
{
	wt_args_db_get_errpfx args;

	wt_args_db_get_errpfx_pack;

	wt_args_db_toc_sched(WT_OP_DB_GET_ERRPFX);
}

static int __wt_api_db_open(
	DB *db,
	WT_TOC *toc,
	const char *dbname,
	mode_t mode,
	u_int32_t flags);
static int __wt_api_db_open(
	DB *db,
	WT_TOC *toc,
	const char *dbname,
	mode_t mode,
	u_int32_t flags)
{
	wt_args_db_open args;

	wt_args_db_open_pack;

	wt_args_db_toc_sched(WT_OP_DB_OPEN);
}

static int __wt_api_db_set_btree_compare(
	DB *db,
	WT_TOC *toc,
	int (*btree_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_set_btree_compare(
	DB *db,
	WT_TOC *toc,
	int (*btree_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_set_btree_compare args;

	wt_args_db_set_btree_compare_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_BTREE_COMPARE);
}

static int __wt_api_db_set_btree_compare_int(
	DB *db,
	WT_TOC *toc,
	int btree_compare_int);
static int __wt_api_db_set_btree_compare_int(
	DB *db,
	WT_TOC *toc,
	int btree_compare_int)
{
	wt_args_db_set_btree_compare_int args;

	wt_args_db_set_btree_compare_int_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_BTREE_COMPARE_INT);
}

static int __wt_api_db_set_btree_dup_compare(
	DB *db,
	WT_TOC *toc,
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_set_btree_dup_compare(
	DB *db,
	WT_TOC *toc,
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *))
{
	wt_args_db_set_btree_dup_compare args;

	wt_args_db_set_btree_dup_compare_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_BTREE_DUP_COMPARE);
}

static int __wt_api_db_set_btree_dup_offpage(
	DB *db,
	WT_TOC *toc,
	u_int32_t btree_dup_offpage);
static int __wt_api_db_set_btree_dup_offpage(
	DB *db,
	WT_TOC *toc,
	u_int32_t btree_dup_offpage)
{
	wt_args_db_set_btree_dup_offpage args;

	wt_args_db_set_btree_dup_offpage_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_BTREE_DUP_OFFPAGE);
}

static int __wt_api_db_set_btree_itemsize(
	DB *db,
	WT_TOC *toc,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize);
static int __wt_api_db_set_btree_itemsize(
	DB *db,
	WT_TOC *toc,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize)
{
	wt_args_db_set_btree_itemsize args;

	wt_args_db_set_btree_itemsize_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_BTREE_ITEMSIZE);
}

static int __wt_api_db_set_btree_pagesize(
	DB *db,
	WT_TOC *toc,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize);
static int __wt_api_db_set_btree_pagesize(
	DB *db,
	WT_TOC *toc,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize)
{
	wt_args_db_set_btree_pagesize args;

	wt_args_db_set_btree_pagesize_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_BTREE_PAGESIZE);
}

static int __wt_api_db_set_errcall(
	DB *db,
	WT_TOC *toc,
	void (*errcall)(const DB *, const char *));
static int __wt_api_db_set_errcall(
	DB *db,
	WT_TOC *toc,
	void (*errcall)(const DB *, const char *))
{
	wt_args_db_set_errcall args;

	wt_args_db_set_errcall_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_ERRCALL);
}

static int __wt_api_db_set_errfile(
	DB *db,
	WT_TOC *toc,
	FILE *errfile);
static int __wt_api_db_set_errfile(
	DB *db,
	WT_TOC *toc,
	FILE *errfile)
{
	wt_args_db_set_errfile args;

	wt_args_db_set_errfile_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_ERRFILE);
}

static int __wt_api_db_set_errpfx(
	DB *db,
	WT_TOC *toc,
	const char *errpfx);
static int __wt_api_db_set_errpfx(
	DB *db,
	WT_TOC *toc,
	const char *errpfx)
{
	wt_args_db_set_errpfx args;

	wt_args_db_set_errpfx_pack;

	wt_args_db_toc_sched(WT_OP_DB_SET_ERRPFX);
}

static int __wt_api_db_stat_clear(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_db_stat_clear(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_db_stat_clear args;

	wt_args_db_stat_clear_pack;

	wt_args_db_toc_sched(WT_OP_DB_STAT_CLEAR);
}

static int __wt_api_db_stat_print(
	DB *db,
	WT_TOC *toc,
	FILE * stream,
	u_int32_t flags);
static int __wt_api_db_stat_print(
	DB *db,
	WT_TOC *toc,
	FILE * stream,
	u_int32_t flags)
{
	wt_args_db_stat_print args;

	wt_args_db_stat_print_pack;

	wt_args_db_toc_sched(WT_OP_DB_STAT_PRINT);
}

static int __wt_api_db_sync(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_db_sync(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_db_sync args;

	wt_args_db_sync_pack;

	wt_args_db_toc_sched(WT_OP_DB_SYNC);
}

static int __wt_api_db_verify(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_db_verify(
	DB *db,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_db_verify args;

	wt_args_db_verify_pack;

	wt_args_db_toc_sched(WT_OP_DB_VERIFY);
}

static int __wt_api_env_close(
	ENV *env,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_env_close(
	ENV *env,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_env_close args;

	wt_args_env_close_pack;

	wt_args_env_toc_sched(WT_OP_ENV_CLOSE);
}

static int __wt_api_env_destroy(
	ENV *env,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_env_destroy(
	ENV *env,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_env_destroy args;

	wt_args_env_destroy_pack;

	wt_args_env_toc_sched(WT_OP_ENV_DESTROY);
}

static int __wt_api_env_get_cachesize(
	ENV *env,
	WT_TOC *toc,
	u_int32_t *cachesize);
static int __wt_api_env_get_cachesize(
	ENV *env,
	WT_TOC *toc,
	u_int32_t *cachesize)
{
	wt_args_env_get_cachesize args;

	wt_args_env_get_cachesize_pack;

	wt_args_env_toc_sched(WT_OP_ENV_GET_CACHESIZE);
}

static int __wt_api_env_get_errcall(
	ENV *env,
	WT_TOC *toc,
	void (**errcall)(const ENV *, const char *));
static int __wt_api_env_get_errcall(
	ENV *env,
	WT_TOC *toc,
	void (**errcall)(const ENV *, const char *))
{
	wt_args_env_get_errcall args;

	wt_args_env_get_errcall_pack;

	wt_args_env_toc_sched(WT_OP_ENV_GET_ERRCALL);
}

static int __wt_api_env_get_errfile(
	ENV *env,
	WT_TOC *toc,
	FILE **errfile);
static int __wt_api_env_get_errfile(
	ENV *env,
	WT_TOC *toc,
	FILE **errfile)
{
	wt_args_env_get_errfile args;

	wt_args_env_get_errfile_pack;

	wt_args_env_toc_sched(WT_OP_ENV_GET_ERRFILE);
}

static int __wt_api_env_get_errpfx(
	ENV *env,
	WT_TOC *toc,
	const char **errpfx);
static int __wt_api_env_get_errpfx(
	ENV *env,
	WT_TOC *toc,
	const char **errpfx)
{
	wt_args_env_get_errpfx args;

	wt_args_env_get_errpfx_pack;

	wt_args_env_toc_sched(WT_OP_ENV_GET_ERRPFX);
}

static int __wt_api_env_get_verbose(
	ENV *env,
	WT_TOC *toc,
	u_int32_t *verbose);
static int __wt_api_env_get_verbose(
	ENV *env,
	WT_TOC *toc,
	u_int32_t *verbose)
{
	wt_args_env_get_verbose args;

	wt_args_env_get_verbose_pack;

	wt_args_env_toc_sched(WT_OP_ENV_GET_VERBOSE);
}

static int __wt_api_env_open(
	ENV *env,
	WT_TOC *toc,
	const char *home,
	mode_t mode,
	u_int32_t flags);
static int __wt_api_env_open(
	ENV *env,
	WT_TOC *toc,
	const char *home,
	mode_t mode,
	u_int32_t flags)
{
	wt_args_env_open args;

	wt_args_env_open_pack;

	wt_args_env_toc_sched(WT_OP_ENV_OPEN);
}

static int __wt_api_env_set_cachesize(
	ENV *env,
	WT_TOC *toc,
	u_int32_t cachesize);
static int __wt_api_env_set_cachesize(
	ENV *env,
	WT_TOC *toc,
	u_int32_t cachesize)
{
	wt_args_env_set_cachesize args;

	wt_args_env_set_cachesize_pack;

	wt_args_env_toc_sched(WT_OP_ENV_SET_CACHESIZE);
}

static int __wt_api_env_set_errcall(
	ENV *env,
	WT_TOC *toc,
	void (*errcall)(const ENV *, const char *));
static int __wt_api_env_set_errcall(
	ENV *env,
	WT_TOC *toc,
	void (*errcall)(const ENV *, const char *))
{
	wt_args_env_set_errcall args;

	wt_args_env_set_errcall_pack;

	wt_args_env_toc_sched(WT_OP_ENV_SET_ERRCALL);
}

static int __wt_api_env_set_errfile(
	ENV *env,
	WT_TOC *toc,
	FILE *errfile);
static int __wt_api_env_set_errfile(
	ENV *env,
	WT_TOC *toc,
	FILE *errfile)
{
	wt_args_env_set_errfile args;

	wt_args_env_set_errfile_pack;

	wt_args_env_toc_sched(WT_OP_ENV_SET_ERRFILE);
}

static int __wt_api_env_set_errpfx(
	ENV *env,
	WT_TOC *toc,
	const char *errpfx);
static int __wt_api_env_set_errpfx(
	ENV *env,
	WT_TOC *toc,
	const char *errpfx)
{
	wt_args_env_set_errpfx args;

	wt_args_env_set_errpfx_pack;

	wt_args_env_toc_sched(WT_OP_ENV_SET_ERRPFX);
}

static int __wt_api_env_set_verbose(
	ENV *env,
	WT_TOC *toc,
	u_int32_t verbose);
static int __wt_api_env_set_verbose(
	ENV *env,
	WT_TOC *toc,
	u_int32_t verbose)
{
	wt_args_env_set_verbose args;

	wt_args_env_set_verbose_pack;

	wt_args_env_toc_sched(WT_OP_ENV_SET_VERBOSE);
}

static int __wt_api_env_stat_clear(
	ENV *env,
	WT_TOC *toc,
	u_int32_t flags);
static int __wt_api_env_stat_clear(
	ENV *env,
	WT_TOC *toc,
	u_int32_t flags)
{
	wt_args_env_stat_clear args;

	wt_args_env_stat_clear_pack;

	wt_args_env_toc_sched(WT_OP_ENV_STAT_CLEAR);
}

static int __wt_api_env_stat_print(
	ENV *env,
	WT_TOC *toc,
	FILE *stream,
	u_int32_t flags);
static int __wt_api_env_stat_print(
	ENV *env,
	WT_TOC *toc,
	FILE *stream,
	u_int32_t flags)
{
	wt_args_env_stat_print args;

	wt_args_env_stat_print_pack;

	wt_args_env_toc_sched(WT_OP_ENV_STAT_PRINT);
}

static int __wt_db_get_btree_compare(WT_TOC *toc);
static int __wt_db_get_btree_compare(WT_TOC *toc)
{
	wt_args_db_get_btree_compare_unpack;

	*(btree_compare) = db->btree_compare;
	return (0);
}

static int __wt_db_get_btree_compare_int(WT_TOC *toc);
static int __wt_db_get_btree_compare_int(WT_TOC *toc)
{
	wt_args_db_get_btree_compare_int_unpack;

	*(btree_compare_int) = db->btree_compare_int;
	return (0);
}

static int __wt_db_get_btree_dup_compare(WT_TOC *toc);
static int __wt_db_get_btree_dup_compare(WT_TOC *toc)
{
	wt_args_db_get_btree_dup_compare_unpack;

	*(btree_dup_compare) = db->btree_dup_compare;
	return (0);
}

static int __wt_db_get_btree_dup_offpage(WT_TOC *toc);
static int __wt_db_get_btree_dup_offpage(WT_TOC *toc)
{
	wt_args_db_get_btree_dup_offpage_unpack;

	*(btree_dup_offpage) = db->btree_dup_offpage;
	return (0);
}

static int __wt_db_get_btree_itemsize(WT_TOC *toc);
static int __wt_db_get_btree_itemsize(WT_TOC *toc)
{
	wt_args_db_get_btree_itemsize_unpack;

	*(intlitemsize) = db->intlitemsize;
	*(leafitemsize) = db->leafitemsize;
	return (0);
}

static int __wt_db_get_btree_pagesize(WT_TOC *toc);
static int __wt_db_get_btree_pagesize(WT_TOC *toc)
{
	wt_args_db_get_btree_pagesize_unpack;

	*(allocsize) = db->allocsize;
	*(intlsize) = db->intlsize;
	*(leafsize) = db->leafsize;
	*(extsize) = db->extsize;
	return (0);
}

static int __wt_db_get_errcall(WT_TOC *toc);
static int __wt_db_get_errcall(WT_TOC *toc)
{
	wt_args_db_get_errcall_unpack;

	*(errcall) = db->errcall;
	return (0);
}

static int __wt_db_get_errfile(WT_TOC *toc);
static int __wt_db_get_errfile(WT_TOC *toc)
{
	wt_args_db_get_errfile_unpack;

	*(errfile) = db->errfile;
	return (0);
}

static int __wt_db_get_errpfx(WT_TOC *toc);
static int __wt_db_get_errpfx(WT_TOC *toc)
{
	wt_args_db_get_errpfx_unpack;

	*(errpfx) = db->errpfx;
	return (0);
}

static int __wt_db_set_btree_compare(WT_TOC *toc);
static int __wt_db_set_btree_compare(WT_TOC *toc)
{
	wt_args_db_set_btree_compare_unpack;

	db->btree_compare = btree_compare;
	return (0);
}

static int __wt_db_set_btree_compare_int(WT_TOC *toc);
static int __wt_db_set_btree_compare_int(WT_TOC *toc)
{
	wt_args_db_set_btree_compare_int_unpack;
	int ret;

	if ((ret = __wt_db_set_btree_compare_int_verify(toc)) != 0)
		return (ret);

	db->btree_compare_int = btree_compare_int;
	return (0);
}

static int __wt_db_set_btree_dup_compare(WT_TOC *toc);
static int __wt_db_set_btree_dup_compare(WT_TOC *toc)
{
	wt_args_db_set_btree_dup_compare_unpack;

	db->btree_dup_compare = btree_dup_compare;
	return (0);
}

static int __wt_db_set_btree_dup_offpage(WT_TOC *toc);
static int __wt_db_set_btree_dup_offpage(WT_TOC *toc)
{
	wt_args_db_set_btree_dup_offpage_unpack;

	db->btree_dup_offpage = btree_dup_offpage;
	return (0);
}

static int __wt_db_set_btree_itemsize(WT_TOC *toc);
static int __wt_db_set_btree_itemsize(WT_TOC *toc)
{
	wt_args_db_set_btree_itemsize_unpack;

	db->intlitemsize = intlitemsize;
	db->leafitemsize = leafitemsize;
	return (0);
}

static int __wt_db_set_btree_pagesize(WT_TOC *toc);
static int __wt_db_set_btree_pagesize(WT_TOC *toc)
{
	wt_args_db_set_btree_pagesize_unpack;

	db->allocsize = allocsize;
	db->intlsize = intlsize;
	db->leafsize = leafsize;
	db->extsize = extsize;
	return (0);
}

static int __wt_db_set_errcall(WT_TOC *toc);
static int __wt_db_set_errcall(WT_TOC *toc)
{
	wt_args_db_set_errcall_unpack;

	db->errcall = errcall;
	return (0);
}

static int __wt_db_set_errfile(WT_TOC *toc);
static int __wt_db_set_errfile(WT_TOC *toc)
{
	wt_args_db_set_errfile_unpack;

	db->errfile = errfile;
	return (0);
}

static int __wt_db_set_errpfx(WT_TOC *toc);
static int __wt_db_set_errpfx(WT_TOC *toc)
{
	wt_args_db_set_errpfx_unpack;

	db->errpfx = errpfx;
	return (0);
}

static int __wt_env_get_cachesize(WT_TOC *toc);
static int __wt_env_get_cachesize(WT_TOC *toc)
{
	wt_args_env_get_cachesize_unpack;

	*(cachesize) = env->cachesize;
	return (0);
}

static int __wt_env_get_errcall(WT_TOC *toc);
static int __wt_env_get_errcall(WT_TOC *toc)
{
	wt_args_env_get_errcall_unpack;

	*(errcall) = env->errcall;
	return (0);
}

static int __wt_env_get_errfile(WT_TOC *toc);
static int __wt_env_get_errfile(WT_TOC *toc)
{
	wt_args_env_get_errfile_unpack;

	*(errfile) = env->errfile;
	return (0);
}

static int __wt_env_get_errpfx(WT_TOC *toc);
static int __wt_env_get_errpfx(WT_TOC *toc)
{
	wt_args_env_get_errpfx_unpack;

	*(errpfx) = env->errpfx;
	return (0);
}

static int __wt_env_get_verbose(WT_TOC *toc);
static int __wt_env_get_verbose(WT_TOC *toc)
{
	wt_args_env_get_verbose_unpack;

	*(verbose) = env->verbose;
	return (0);
}

static int __wt_env_set_cachesize(WT_TOC *toc);
static int __wt_env_set_cachesize(WT_TOC *toc)
{
	wt_args_env_set_cachesize_unpack;

	env->cachesize = cachesize;
	return (0);
}

static int __wt_env_set_errcall(WT_TOC *toc);
static int __wt_env_set_errcall(WT_TOC *toc)
{
	wt_args_env_set_errcall_unpack;

	env->errcall = errcall;
	return (0);
}

static int __wt_env_set_errfile(WT_TOC *toc);
static int __wt_env_set_errfile(WT_TOC *toc)
{
	wt_args_env_set_errfile_unpack;

	env->errfile = errfile;
	return (0);
}

static int __wt_env_set_errpfx(WT_TOC *toc);
static int __wt_env_set_errpfx(WT_TOC *toc)
{
	wt_args_env_set_errpfx_unpack;

	env->errpfx = errpfx;
	return (0);
}

static int __wt_env_set_verbose(WT_TOC *toc);
static int __wt_env_set_verbose(WT_TOC *toc)
{
	wt_args_env_set_verbose_unpack;
	int ret;

	if ((ret = __wt_env_set_verbose_verify(toc)) != 0)
		return (ret);

	env->verbose = verbose;
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
	    (ENV *, WT_TOC *, u_int32_t ))
	    __wt_env_lockout_err;
	env->err = (void (*)
	    (ENV *, int , const char *, ...))
	    __wt_env_lockout_err;
	env->errx = (void (*)
	    (ENV *, const char *, ...))
	    __wt_env_lockout_err;
	env->get_cachesize = (int (*)
	    (ENV *, WT_TOC *, u_int32_t *))
	    __wt_env_lockout_err;
	env->get_errcall = (int (*)
	    (ENV *, WT_TOC *, void (**)(const ENV *, const char *)))
	    __wt_env_lockout_err;
	env->get_errfile = (int (*)
	    (ENV *, WT_TOC *, FILE **))
	    __wt_env_lockout_err;
	env->get_errpfx = (int (*)
	    (ENV *, WT_TOC *, const char **))
	    __wt_env_lockout_err;
	env->get_verbose = (int (*)
	    (ENV *, WT_TOC *, u_int32_t *))
	    __wt_env_lockout_err;
	env->open = (int (*)
	    (ENV *, WT_TOC *, const char *, mode_t , u_int32_t ))
	    __wt_env_lockout_err;
	env->set_cachesize = (int (*)
	    (ENV *, WT_TOC *, u_int32_t ))
	    __wt_env_lockout_err;
	env->set_errcall = (int (*)
	    (ENV *, WT_TOC *, void (*)(const ENV *, const char *)))
	    __wt_env_lockout_err;
	env->set_errfile = (int (*)
	    (ENV *, WT_TOC *, FILE *))
	    __wt_env_lockout_err;
	env->set_errpfx = (int (*)
	    (ENV *, WT_TOC *, const char *))
	    __wt_env_lockout_err;
	env->set_verbose = (int (*)
	    (ENV *, WT_TOC *, u_int32_t ))
	    __wt_env_lockout_err;
	env->stat_clear = (int (*)
	    (ENV *, WT_TOC *, u_int32_t ))
	    __wt_env_lockout_err;
	env->stat_print = (int (*)
	    (ENV *, WT_TOC *, FILE *, u_int32_t ))
	    __wt_env_lockout_err;
}

void
__wt_db_config_methods(DB *db)
{
	db->bulk_load = __wt_api_db_bulk_load;
	db->close = __wt_api_db_close;
	db->destroy = __wt_api_db_destroy;
	db->dump = (int (*)
	    (DB *, WT_TOC *, FILE *, u_int32_t ))
	    __wt_db_lockout_open;
	db->err = __wt_db_err;
	db->errx = __wt_db_errx;
	db->get = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, DBT *, u_int32_t ))
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
	    (DB *, WT_TOC *, u_int32_t ))
	    __wt_db_lockout_open;
	db->verify = (int (*)
	    (DB *, WT_TOC *, u_int32_t ))
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
	    (DB *, WT_TOC *, u_int32_t , int (*)(DB *, DBT **, DBT **)))
	    __wt_db_lockout_err;
	db->close = (int (*)
	    (DB *, WT_TOC *, u_int32_t ))
	    __wt_db_lockout_err;
	db->dump = (int (*)
	    (DB *, WT_TOC *, FILE *, u_int32_t ))
	    __wt_db_lockout_err;
	db->err = (void (*)
	    (DB *, int , const char *, ...))
	    __wt_db_lockout_err;
	db->errx = (void (*)
	    (DB *, const char *, ...))
	    __wt_db_lockout_err;
	db->get = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout_err;
	db->get_btree_compare = (int (*)
	    (DB *, WT_TOC *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->get_btree_compare_int = (int (*)
	    (DB *, WT_TOC *, int *))
	    __wt_db_lockout_err;
	db->get_btree_dup_compare = (int (*)
	    (DB *, WT_TOC *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->get_btree_dup_offpage = (int (*)
	    (DB *, WT_TOC *, u_int32_t *))
	    __wt_db_lockout_err;
	db->get_btree_itemsize = (int (*)
	    (DB *, WT_TOC *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout_err;
	db->get_btree_pagesize = (int (*)
	    (DB *, WT_TOC *, u_int32_t *, u_int32_t *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout_err;
	db->get_errcall = (int (*)
	    (DB *, WT_TOC *, void (**)(const DB *, const char *)))
	    __wt_db_lockout_err;
	db->get_errfile = (int (*)
	    (DB *, WT_TOC *, FILE **))
	    __wt_db_lockout_err;
	db->get_errpfx = (int (*)
	    (DB *, WT_TOC *, const char **))
	    __wt_db_lockout_err;
	db->open = (int (*)
	    (DB *, WT_TOC *, const char *, mode_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->set_btree_compare = (int (*)
	    (DB *, WT_TOC *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->set_btree_compare_int = (int (*)
	    (DB *, WT_TOC *, int ))
	    __wt_db_lockout_err;
	db->set_btree_dup_compare = (int (*)
	    (DB *, WT_TOC *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->set_btree_dup_offpage = (int (*)
	    (DB *, WT_TOC *, u_int32_t ))
	    __wt_db_lockout_err;
	db->set_btree_itemsize = (int (*)
	    (DB *, WT_TOC *, u_int32_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->set_btree_pagesize = (int (*)
	    (DB *, WT_TOC *, u_int32_t , u_int32_t , u_int32_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->set_errcall = (int (*)
	    (DB *, WT_TOC *, void (*)(const DB *, const char *)))
	    __wt_db_lockout_err;
	db->set_errfile = (int (*)
	    (DB *, WT_TOC *, FILE *))
	    __wt_db_lockout_err;
	db->set_errpfx = (int (*)
	    (DB *, WT_TOC *, const char *))
	    __wt_db_lockout_err;
	db->stat_clear = (int (*)
	    (DB *, WT_TOC *, u_int32_t ))
	    __wt_db_lockout_err;
	db->stat_print = (int (*)
	    (DB *, WT_TOC *, FILE * , u_int32_t ))
	    __wt_db_lockout_err;
	db->sync = (int (*)
	    (DB *, WT_TOC *, u_int32_t ))
	    __wt_db_lockout_err;
	db->verify = (int (*)
	    (DB *, WT_TOC *, u_int32_t ))
	    __wt_db_lockout_err;
}

void
__wt_api_switch(WT_TOC *toc)
{
	int ret;

	switch (toc->op) {
	case WT_OP_DB_BULK_LOAD:
		ret = __wt_db_bulk_load(toc);
		break;
	case WT_OP_DB_CLOSE:
		ret = __wt_db_close(toc);
		break;
	case WT_OP_DB_DESTROY:
		ret = __wt_db_destroy(toc);
		break;
	case WT_OP_DB_DUMP:
		ret = __wt_db_dump(toc);
		break;
	case WT_OP_DB_GET:
		ret = __wt_db_get(toc);
		break;
	case WT_OP_DB_GET_BTREE_COMPARE:
		ret = __wt_db_get_btree_compare(toc);
		break;
	case WT_OP_DB_GET_BTREE_COMPARE_INT:
		ret = __wt_db_get_btree_compare_int(toc);
		break;
	case WT_OP_DB_GET_BTREE_DUP_COMPARE:
		ret = __wt_db_get_btree_dup_compare(toc);
		break;
	case WT_OP_DB_GET_BTREE_DUP_OFFPAGE:
		ret = __wt_db_get_btree_dup_offpage(toc);
		break;
	case WT_OP_DB_GET_BTREE_ITEMSIZE:
		ret = __wt_db_get_btree_itemsize(toc);
		break;
	case WT_OP_DB_GET_BTREE_PAGESIZE:
		ret = __wt_db_get_btree_pagesize(toc);
		break;
	case WT_OP_DB_GET_ERRCALL:
		ret = __wt_db_get_errcall(toc);
		break;
	case WT_OP_DB_GET_ERRFILE:
		ret = __wt_db_get_errfile(toc);
		break;
	case WT_OP_DB_GET_ERRPFX:
		ret = __wt_db_get_errpfx(toc);
		break;
	case WT_OP_DB_OPEN:
		ret = __wt_db_open(toc);
		break;
	case WT_OP_DB_SET_BTREE_COMPARE:
		ret = __wt_db_set_btree_compare(toc);
		break;
	case WT_OP_DB_SET_BTREE_COMPARE_INT:
		ret = __wt_db_set_btree_compare_int(toc);
		break;
	case WT_OP_DB_SET_BTREE_DUP_COMPARE:
		ret = __wt_db_set_btree_dup_compare(toc);
		break;
	case WT_OP_DB_SET_BTREE_DUP_OFFPAGE:
		ret = __wt_db_set_btree_dup_offpage(toc);
		break;
	case WT_OP_DB_SET_BTREE_ITEMSIZE:
		ret = __wt_db_set_btree_itemsize(toc);
		break;
	case WT_OP_DB_SET_BTREE_PAGESIZE:
		ret = __wt_db_set_btree_pagesize(toc);
		break;
	case WT_OP_DB_SET_ERRCALL:
		ret = __wt_db_set_errcall(toc);
		break;
	case WT_OP_DB_SET_ERRFILE:
		ret = __wt_db_set_errfile(toc);
		break;
	case WT_OP_DB_SET_ERRPFX:
		ret = __wt_db_set_errpfx(toc);
		break;
	case WT_OP_DB_STAT_CLEAR:
		ret = __wt_db_stat_clear(toc);
		break;
	case WT_OP_DB_STAT_PRINT:
		ret = __wt_db_stat_print(toc);
		break;
	case WT_OP_DB_SYNC:
		ret = __wt_db_sync(toc);
		break;
	case WT_OP_DB_VERIFY:
		ret = __wt_db_verify(toc);
		break;
	case WT_OP_ENV_CLOSE:
		ret = __wt_env_close(toc);
		break;
	case WT_OP_ENV_DESTROY:
		ret = __wt_env_destroy(toc);
		break;
	case WT_OP_ENV_GET_CACHESIZE:
		ret = __wt_env_get_cachesize(toc);
		break;
	case WT_OP_ENV_GET_ERRCALL:
		ret = __wt_env_get_errcall(toc);
		break;
	case WT_OP_ENV_GET_ERRFILE:
		ret = __wt_env_get_errfile(toc);
		break;
	case WT_OP_ENV_GET_ERRPFX:
		ret = __wt_env_get_errpfx(toc);
		break;
	case WT_OP_ENV_GET_VERBOSE:
		ret = __wt_env_get_verbose(toc);
		break;
	case WT_OP_ENV_OPEN:
		ret = __wt_env_open(toc);
		break;
	case WT_OP_ENV_SET_CACHESIZE:
		ret = __wt_env_set_cachesize(toc);
		break;
	case WT_OP_ENV_SET_ERRCALL:
		ret = __wt_env_set_errcall(toc);
		break;
	case WT_OP_ENV_SET_ERRFILE:
		ret = __wt_env_set_errfile(toc);
		break;
	case WT_OP_ENV_SET_ERRPFX:
		ret = __wt_env_set_errpfx(toc);
		break;
	case WT_OP_ENV_SET_VERBOSE:
		ret = __wt_env_set_verbose(toc);
		break;
	case WT_OP_ENV_STAT_CLEAR:
		ret = __wt_env_stat_clear(toc);
		break;
	case WT_OP_ENV_STAT_PRINT:
		ret = __wt_env_stat_print(toc);
		break;
	default:
		ret = WT_ERROR;
		break;
	}

	toc->ret = ret;
}
