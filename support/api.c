/* DO NOT EDIT: automatically built by dist/api.py. */

#include "wt_internal.h"

static int __wt_api_db_btree_compare_dup_get(
	DB *db,
	int (**btree_compare_dup)(DB *, const DBT *, const DBT *));
static int __wt_api_db_btree_compare_dup_get(
	DB *db,
	int (**btree_compare_dup)(DB *, const DBT *, const DBT *))
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_DUP_GET);
	*btree_compare_dup = db->btree_compare_dup;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_compare_dup_set(
	DB *db,
	int (*btree_compare_dup)(DB *, const DBT *, const DBT *));
static int __wt_api_db_btree_compare_dup_set(
	DB *db,
	int (*btree_compare_dup)(DB *, const DBT *, const DBT *))
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_DUP_SET);
	db->btree_compare_dup = btree_compare_dup;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_compare_get(
	DB *db,
	int (**btree_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_btree_compare_get(
	DB *db,
	int (**btree_compare)(DB *, const DBT *, const DBT *))
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_GET);
	*btree_compare = db->btree_compare;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_compare_int_get(
	DB *db,
	int *btree_compare_int);
static int __wt_api_db_btree_compare_int_get(
	DB *db,
	int *btree_compare_int)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_INT_GET);
	*btree_compare_int = db->btree_compare_int;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_compare_int_set(
	DB *db,
	int btree_compare_int);
static int __wt_api_db_btree_compare_int_set(
	DB *db,
	int btree_compare_int)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	WT_RET((__wt_db_btree_compare_int_set_verify(
	    db, btree_compare_int)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_INT_SET);
	db->btree_compare_int = btree_compare_int;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_compare_set(
	DB *db,
	int (*btree_compare)(DB *, const DBT *, const DBT *));
static int __wt_api_db_btree_compare_set(
	DB *db,
	int (*btree_compare)(DB *, const DBT *, const DBT *))
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_SET);
	db->btree_compare = btree_compare;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_dup_offpage_get(
	DB *db,
	uint32_t *btree_dup_offpage);
static int __wt_api_db_btree_dup_offpage_get(
	DB *db,
	uint32_t *btree_dup_offpage)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_DUP_OFFPAGE_GET);
	*btree_dup_offpage = db->btree_dup_offpage;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_dup_offpage_set(
	DB *db,
	uint32_t btree_dup_offpage);
static int __wt_api_db_btree_dup_offpage_set(
	DB *db,
	uint32_t btree_dup_offpage)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	WT_RET((__wt_db_btree_dup_offpage_set_verify(
	    db, btree_dup_offpage)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_DUP_OFFPAGE_SET);
	db->btree_dup_offpage = btree_dup_offpage;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_itemsize_get(
	DB *db,
	uint32_t *intlitemsize,
	uint32_t *leafitemsize);
static int __wt_api_db_btree_itemsize_get(
	DB *db,
	uint32_t *intlitemsize,
	uint32_t *leafitemsize)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_ITEMSIZE_GET);
	*intlitemsize = db->intlitemsize;
	*leafitemsize = db->leafitemsize;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_itemsize_set(
	DB *db,
	uint32_t intlitemsize,
	uint32_t leafitemsize);
static int __wt_api_db_btree_itemsize_set(
	DB *db,
	uint32_t intlitemsize,
	uint32_t leafitemsize)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_ITEMSIZE_SET);
	db->intlitemsize = intlitemsize;
	db->leafitemsize = leafitemsize;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_pagesize_get(
	DB *db,
	uint32_t *allocsize,
	uint32_t *intlmin,
	uint32_t *intlmax,
	uint32_t *leafmin,
	uint32_t *leafmax);
static int __wt_api_db_btree_pagesize_get(
	DB *db,
	uint32_t *allocsize,
	uint32_t *intlmin,
	uint32_t *intlmax,
	uint32_t *leafmin,
	uint32_t *leafmax)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_PAGESIZE_GET);
	*allocsize = db->allocsize;
	*intlmin = db->intlmin;
	*intlmax = db->intlmax;
	*leafmin = db->leafmin;
	*leafmax = db->leafmax;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_pagesize_set(
	DB *db,
	uint32_t allocsize,
	uint32_t intlmin,
	uint32_t intlmax,
	uint32_t leafmin,
	uint32_t leafmax);
static int __wt_api_db_btree_pagesize_set(
	DB *db,
	uint32_t allocsize,
	uint32_t intlmin,
	uint32_t intlmax,
	uint32_t leafmin,
	uint32_t leafmax)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_PAGESIZE_SET);
	db->allocsize = allocsize;
	db->intlmin = intlmin;
	db->intlmax = intlmax;
	db->leafmin = leafmin;
	db->leafmax = leafmax;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_bulk_load(
	DB *db,
	uint32_t flags,
	void (*progress)(const char *, uint64_t),
	int (*cb)(DB *, DBT **, DBT **));
static int __wt_api_db_bulk_load(
	DB *db,
	uint32_t flags,
	void (*progress)(const char *, uint64_t),
	int (*cb)(DB *, DBT **, DBT **))
{
	const char *method_name = "DB.bulk_load";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_DB_RDONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_BULK_LOAD);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_BULK_LOAD);
	ret = __wt_db_bulk_load(toc, flags, progress, cb);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_db_close(
	DB *db,
	uint32_t flags);
static int __wt_api_db_close(
	DB *db,
	uint32_t flags)
{
	const char *method_name = "DB.close";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_CLOSE);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_CLOSE);
	ret = __wt_db_close(toc, flags);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_db_col_del(
	DB *db,
	WT_TOC *toc,
	uint64_t recno,
	uint32_t flags);
static int __wt_api_db_col_del(
	DB *db,
	WT_TOC *toc,
	uint64_t recno,
	uint32_t flags)
{
	const char *method_name = "DB.col_del";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_DB_COL_ONLY(db, method_name);
	WT_DB_RDONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_COL_DEL);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_COL_DEL);
	while ((ret = __wt_db_col_del(toc, recno)) == WT_RESTART)
		WT_STAT_INCR(ienv->method_stats, DB_COL_DEL_RESTART);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 0));
	return (ret);
}

static int __wt_api_db_col_get(
	DB *db,
	WT_TOC *toc,
	uint64_t recno,
	DBT *data,
	uint32_t flags);
static int __wt_api_db_col_get(
	DB *db,
	WT_TOC *toc,
	uint64_t recno,
	DBT *data,
	uint32_t flags)
{
	const char *method_name = "DB.col_get";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_DB_COL_ONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_COL_GET);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_COL_GET);
	ret = __wt_db_col_get(toc, recno, data);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 0));
	return (ret);
}

static int __wt_api_db_col_put(
	DB *db,
	WT_TOC *toc,
	uint64_t recno,
	DBT *data,
	uint32_t flags);
static int __wt_api_db_col_put(
	DB *db,
	WT_TOC *toc,
	uint64_t recno,
	DBT *data,
	uint32_t flags)
{
	const char *method_name = "DB.col_put";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_DB_COL_ONLY(db, method_name);
	WT_DB_RDONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_COL_PUT);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_COL_PUT);
	while ((ret = __wt_db_col_put(toc, recno, data)) == WT_RESTART)
		WT_STAT_INCR(ienv->method_stats, DB_COL_PUT_RESTART);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 0));
	return (ret);
}

static int __wt_api_db_column_set(
	DB *db,
	uint32_t fixed_len,
	const char *dictionary,
	uint32_t flags);
static int __wt_api_db_column_set(
	DB *db,
	uint32_t fixed_len,
	const char *dictionary,
	uint32_t flags)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	WT_ENV_FCHK(env, "DB.column_set",
	    flags, WT_APIMASK_DB_COLUMN_SET);

	WT_RET((__wt_db_column_set_verify(
	    db, fixed_len, dictionary, flags)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_COLUMN_SET);
	db->fixed_len = fixed_len;
	db->dictionary = dictionary;
	db->flags = flags;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_dump(
	DB *db,
	FILE *stream,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_db_dump(
	DB *db,
	FILE *stream,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "DB.dump";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_DUMP);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_DUMP);
	ret = __wt_db_dump(toc, stream, progress, flags);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_db_errcall_get(
	DB *db,
	void (**errcall)(const DB *, const char *));
static int __wt_api_db_errcall_get(
	DB *db,
	void (**errcall)(const DB *, const char *))
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_ERRCALL_GET);
	*errcall = db->errcall;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_errcall_set(
	DB *db,
	void (*errcall)(const DB *, const char *));
static int __wt_api_db_errcall_set(
	DB *db,
	void (*errcall)(const DB *, const char *))
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_ERRCALL_SET);
	db->errcall = errcall;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_errfile_get(
	DB *db,
	FILE **errfile);
static int __wt_api_db_errfile_get(
	DB *db,
	FILE **errfile)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_ERRFILE_GET);
	*errfile = db->errfile;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_errfile_set(
	DB *db,
	FILE *errfile);
static int __wt_api_db_errfile_set(
	DB *db,
	FILE *errfile)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_ERRFILE_SET);
	db->errfile = errfile;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_errpfx_get(
	DB *db,
	const char **errpfx);
static int __wt_api_db_errpfx_get(
	DB *db,
	const char **errpfx)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_ERRPFX_GET);
	*errpfx = db->errpfx;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_errpfx_set(
	DB *db,
	const char *errpfx);
static int __wt_api_db_errpfx_set(
	DB *db,
	const char *errpfx)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_ERRPFX_SET);
	db->errpfx = errpfx;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_db_huffman_set(
	DB *db,
	uint8_t const *huffman_table,
	u_int huffman_table_size,
	uint32_t huffman_flags);
static int __wt_api_db_huffman_set(
	DB *db,
	uint8_t const *huffman_table,
	u_int huffman_table_size,
	uint32_t huffman_flags)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, "DB.huffman_set",
	    huffman_flags, WT_APIMASK_DB_HUFFMAN_SET);

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_HUFFMAN_SET);
	ret = __wt_db_huffman_set(
	    db, huffman_table, huffman_table_size, huffman_flags);
	__wt_unlock(env, ienv->mtx);
	return (ret);
}

static int __wt_api_db_open(
	DB *db,
	const char *name,
	mode_t mode,
	uint32_t flags);
static int __wt_api_db_open(
	DB *db,
	const char *name,
	mode_t mode,
	uint32_t flags)
{
	const char *method_name = "DB.open";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_OPEN);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_OPEN);
	ret = __wt_db_open(toc, name, mode, flags);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_db_row_del(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	uint32_t flags);
static int __wt_api_db_row_del(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	uint32_t flags)
{
	const char *method_name = "DB.row_del";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_DB_ROW_ONLY(db, method_name);
	WT_DB_RDONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_ROW_DEL);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_ROW_DEL);
	while ((ret = __wt_db_row_del(toc, key)) == WT_RESTART)
		WT_STAT_INCR(ienv->method_stats, DB_ROW_DEL_RESTART);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 0));
	return (ret);
}

static int __wt_api_db_row_get(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *data,
	uint32_t flags);
static int __wt_api_db_row_get(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *data,
	uint32_t flags)
{
	const char *method_name = "DB.row_get";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_DB_ROW_ONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_ROW_GET);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_ROW_GET);
	ret = __wt_db_row_get(toc, key, data);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 0));
	return (ret);
}

static int __wt_api_db_row_put(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *data,
	uint32_t flags);
static int __wt_api_db_row_put(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *data,
	uint32_t flags)
{
	const char *method_name = "DB.row_put";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_DB_ROW_ONLY(db, method_name);
	WT_DB_RDONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_ROW_PUT);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_ROW_PUT);
	while ((ret = __wt_db_row_put(toc, key, data)) == WT_RESTART)
		WT_STAT_INCR(ienv->method_stats, DB_ROW_PUT_RESTART);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 0));
	return (ret);
}

static int __wt_api_db_stat_clear(
	DB *db,
	uint32_t flags);
static int __wt_api_db_stat_clear(
	DB *db,
	uint32_t flags)
{
	const char *method_name = "DB.stat_clear";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_STAT_CLEAR);
	WT_STAT_INCR(ienv->method_stats, DB_STAT_CLEAR);
	ret = __wt_db_stat_clear(db);
	return (ret);
}

static int __wt_api_db_stat_print(
	DB *db,
	FILE *stream,
	uint32_t flags);
static int __wt_api_db_stat_print(
	DB *db,
	FILE *stream,
	uint32_t flags)
{
	const char *method_name = "DB.stat_print";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_STAT_PRINT);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_STAT_PRINT);
	ret = __wt_db_stat_print(toc, stream);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_db_sync(
	DB *db,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_db_sync(
	DB *db,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "DB.sync";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_DB_RDONLY(db, method_name);
	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_SYNC);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_SYNC);
	ret = __wt_db_sync(toc, progress, flags);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_db_verify(
	DB *db,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_db_verify(
	DB *db,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "DB.verify";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	WT_TOC *toc = NULL;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_VERIFY);
	WT_RET(__wt_toc_api_set(env, method_name, db, &toc));
	WT_STAT_INCR(ienv->method_stats, DB_VERIFY);
	ret = __wt_db_verify(toc, progress);
	WT_TRET(__wt_toc_api_clr(toc, method_name, 1));
	return (ret);
}

static int __wt_api_env_cache_drain_cnt_get(
	ENV *env,
	uint32_t *cache_drain_cnt);
static int __wt_api_env_cache_drain_cnt_get(
	ENV *env,
	uint32_t *cache_drain_cnt)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_DRAIN_CNT_GET);
	*cache_drain_cnt = env->cache_drain_cnt;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_drain_cnt_set(
	ENV *env,
	uint32_t cache_drain_cnt);
static int __wt_api_env_cache_drain_cnt_set(
	ENV *env,
	uint32_t cache_drain_cnt)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_DRAIN_CNT_SET);
	env->cache_drain_cnt = cache_drain_cnt;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_hash_size_get(
	ENV *env,
	uint32_t *cache_hash_size);
static int __wt_api_env_cache_hash_size_get(
	ENV *env,
	uint32_t *cache_hash_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_HASH_SIZE_GET);
	*cache_hash_size = env->cache_hash_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_hash_size_set(
	ENV *env,
	uint32_t cache_hash_size);
static int __wt_api_env_cache_hash_size_set(
	ENV *env,
	uint32_t cache_hash_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_cache_hash_size_set_verify(
	    env, cache_hash_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_HASH_SIZE_SET);
	env->cache_hash_size = cache_hash_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_size_get(
	ENV *env,
	uint32_t *cache_size);
static int __wt_api_env_cache_size_get(
	ENV *env,
	uint32_t *cache_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_SIZE_GET);
	*cache_size = env->cache_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_size_set(
	ENV *env,
	uint32_t cache_size);
static int __wt_api_env_cache_size_set(
	ENV *env,
	uint32_t cache_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_cache_size_set_verify(
	    env, cache_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_SIZE_SET);
	env->cache_size = cache_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_close(
	ENV *env,
	uint32_t flags);
static int __wt_api_env_close(
	ENV *env,
	uint32_t flags)
{
	const char *method_name = "ENV.close";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_CLOSE);
	WT_STAT_INCR(ienv->method_stats, ENV_CLOSE);
	ret = __wt_env_close(env);
	return (ret);
}

static int __wt_api_env_data_update_initial_get(
	ENV *env,
	uint32_t *data_update_initial);
static int __wt_api_env_data_update_initial_get(
	ENV *env,
	uint32_t *data_update_initial)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_DATA_UPDATE_INITIAL_GET);
	*data_update_initial = env->data_update_initial;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_data_update_initial_set(
	ENV *env,
	uint32_t data_update_initial);
static int __wt_api_env_data_update_initial_set(
	ENV *env,
	uint32_t data_update_initial)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_DATA_UPDATE_INITIAL_SET);
	env->data_update_initial = data_update_initial;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_data_update_max_get(
	ENV *env,
	uint32_t *data_update_max);
static int __wt_api_env_data_update_max_get(
	ENV *env,
	uint32_t *data_update_max)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_DATA_UPDATE_MAX_GET);
	*data_update_max = env->data_update_max;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_data_update_max_set(
	ENV *env,
	uint32_t data_update_max);
static int __wt_api_env_data_update_max_set(
	ENV *env,
	uint32_t data_update_max)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_DATA_UPDATE_MAX_SET);
	env->data_update_max = data_update_max;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_db(
	ENV *env,
	uint32_t flags,
	DB **dbp);
static int __wt_api_env_db(
	ENV *env,
	uint32_t flags,
	DB **dbp)
{
	const char *method_name = "ENV.db";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_DB);
	WT_STAT_INCR(ienv->method_stats, ENV_DB);
	ret = __wt_env_db(env, dbp);
	return (ret);
}

static int __wt_api_env_errcall_get(
	ENV *env,
	void (**errcall)(const ENV *, const char *));
static int __wt_api_env_errcall_get(
	ENV *env,
	void (**errcall)(const ENV *, const char *))
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_ERRCALL_GET);
	*errcall = env->errcall;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_errcall_set(
	ENV *env,
	void (*errcall)(const ENV *, const char *));
static int __wt_api_env_errcall_set(
	ENV *env,
	void (*errcall)(const ENV *, const char *))
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_ERRCALL_SET);
	env->errcall = errcall;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_errfile_get(
	ENV *env,
	FILE **errfile);
static int __wt_api_env_errfile_get(
	ENV *env,
	FILE **errfile)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_ERRFILE_GET);
	*errfile = env->errfile;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_errfile_set(
	ENV *env,
	FILE *errfile);
static int __wt_api_env_errfile_set(
	ENV *env,
	FILE *errfile)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_ERRFILE_SET);
	env->errfile = errfile;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_errpfx_get(
	ENV *env,
	const char **errpfx);
static int __wt_api_env_errpfx_get(
	ENV *env,
	const char **errpfx)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_ERRPFX_GET);
	*errpfx = env->errpfx;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_errpfx_set(
	ENV *env,
	const char *errpfx);
static int __wt_api_env_errpfx_set(
	ENV *env,
	const char *errpfx)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_ERRPFX_SET);
	env->errpfx = errpfx;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_hazard_size_get(
	ENV *env,
	uint32_t *hazard_size);
static int __wt_api_env_hazard_size_get(
	ENV *env,
	uint32_t *hazard_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_HAZARD_SIZE_GET);
	*hazard_size = env->hazard_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_hazard_size_set(
	ENV *env,
	uint32_t hazard_size);
static int __wt_api_env_hazard_size_set(
	ENV *env,
	uint32_t hazard_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_hazard_size_set_verify(
	    env, hazard_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_HAZARD_SIZE_SET);
	env->hazard_size = hazard_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_msgcall_get(
	ENV *env,
	void (**msgcall)(const ENV *, const char *));
static int __wt_api_env_msgcall_get(
	ENV *env,
	void (**msgcall)(const ENV *, const char *))
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_MSGCALL_GET);
	*msgcall = env->msgcall;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_msgcall_set(
	ENV *env,
	void (*msgcall)(const ENV *, const char *));
static int __wt_api_env_msgcall_set(
	ENV *env,
	void (*msgcall)(const ENV *, const char *))
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_MSGCALL_SET);
	env->msgcall = msgcall;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_msgfile_get(
	ENV *env,
	FILE **msgfile);
static int __wt_api_env_msgfile_get(
	ENV *env,
	FILE **msgfile)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_MSGFILE_GET);
	*msgfile = env->msgfile;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_msgfile_set(
	ENV *env,
	FILE *msgfile);
static int __wt_api_env_msgfile_set(
	ENV *env,
	FILE *msgfile)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_MSGFILE_SET);
	env->msgfile = msgfile;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_open(
	ENV *env,
	const char *home,
	mode_t mode,
	uint32_t flags);
static int __wt_api_env_open(
	ENV *env,
	const char *home,
	mode_t mode,
	uint32_t flags)
{
	const char *method_name = "ENV.open";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_OPEN);
	WT_STAT_INCR(ienv->method_stats, ENV_OPEN);
	ret = __wt_env_open(env, home, mode);
	return (ret);
}

static int __wt_api_env_stat_clear(
	ENV *env,
	uint32_t flags);
static int __wt_api_env_stat_clear(
	ENV *env,
	uint32_t flags)
{
	const char *method_name = "ENV.stat_clear";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_STAT_CLEAR);
	WT_STAT_INCR(ienv->method_stats, ENV_STAT_CLEAR);
	ret = __wt_env_stat_clear(env);
	return (ret);
}

static int __wt_api_env_stat_print(
	ENV *env,
	FILE *stream,
	uint32_t flags);
static int __wt_api_env_stat_print(
	ENV *env,
	FILE *stream,
	uint32_t flags)
{
	const char *method_name = "ENV.stat_print";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_STAT_PRINT);
	WT_STAT_INCR(ienv->method_stats, ENV_STAT_PRINT);
	ret = __wt_env_stat_print(env, stream);
	return (ret);
}

static int __wt_api_env_sync(
	ENV *env,
	void (*progress)(const char *, uint64_t),
	uint32_t flags);
static int __wt_api_env_sync(
	ENV *env,
	void (*progress)(const char *, uint64_t),
	uint32_t flags)
{
	const char *method_name = "ENV.sync";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_SYNC);
	WT_STAT_INCR(ienv->method_stats, ENV_SYNC);
	ret = __wt_env_sync(env, progress);
	return (ret);
}

static int __wt_api_env_toc(
	ENV *env,
	uint32_t flags,
	WT_TOC **tocp);
static int __wt_api_env_toc(
	ENV *env,
	uint32_t flags,
	WT_TOC **tocp)
{
	const char *method_name = "ENV.toc";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_TOC);
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_TOC);
	ret = __wt_env_toc(env, tocp);
	__wt_unlock(env, ienv->mtx);
	return (ret);
}

static int __wt_api_env_toc_size_get(
	ENV *env,
	uint32_t *toc_size);
static int __wt_api_env_toc_size_get(
	ENV *env,
	uint32_t *toc_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_TOC_SIZE_GET);
	*toc_size = env->toc_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_toc_size_set(
	ENV *env,
	uint32_t toc_size);
static int __wt_api_env_toc_size_set(
	ENV *env,
	uint32_t toc_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_toc_size_set_verify(
	    env, toc_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_TOC_SIZE_SET);
	env->toc_size = toc_size;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_verbose_get(
	ENV *env,
	uint32_t *verbose);
static int __wt_api_env_verbose_get(
	ENV *env,
	uint32_t *verbose)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_VERBOSE_GET);
	*verbose = env->verbose;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_env_verbose_set(
	ENV *env,
	uint32_t verbose);
static int __wt_api_env_verbose_set(
	ENV *env,
	uint32_t verbose)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_verbose_set_verify(
	    env, verbose)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_VERBOSE_SET);
	env->verbose = verbose;
	__wt_unlock(env, ienv->mtx);
	return (0);
}

static int __wt_api_wt_toc_close(
	WT_TOC *wt_toc,
	uint32_t flags);
static int __wt_api_wt_toc_close(
	WT_TOC *wt_toc,
	uint32_t flags)
{
	const char *method_name = "WT_TOC.close";
	ENV *env = wt_toc->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_WT_TOC_CLOSE);
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, WT_TOC_CLOSE);
	ret = __wt_wt_toc_close(wt_toc);
	__wt_unlock(env, ienv->mtx);
	return (ret);
}

void
__wt_methods_db_config_default(DB *db)
{
	db->btree_compare_dup = __wt_bt_lex_compare;
	db->btree_compare = __wt_bt_lex_compare;
}

void
__wt_methods_db_lockout(DB *db)
{
	db->btree_compare_dup_get = (int (*)
	    (DB *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout;
	db->btree_compare_dup_set = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout;
	db->btree_compare_get = (int (*)
	    (DB *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout;
	db->btree_compare_int_get = (int (*)
	    (DB *, int *))
	    __wt_db_lockout;
	db->btree_compare_int_set = (int (*)
	    (DB *, int ))
	    __wt_db_lockout;
	db->btree_compare_set = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout;
	db->btree_dup_offpage_get = (int (*)
	    (DB *, uint32_t *))
	    __wt_db_lockout;
	db->btree_dup_offpage_set = (int (*)
	    (DB *, uint32_t ))
	    __wt_db_lockout;
	db->btree_itemsize_get = (int (*)
	    (DB *, uint32_t *, uint32_t *))
	    __wt_db_lockout;
	db->btree_itemsize_set = (int (*)
	    (DB *, uint32_t , uint32_t ))
	    __wt_db_lockout;
	db->btree_pagesize_get = (int (*)
	    (DB *, uint32_t *, uint32_t *, uint32_t *, uint32_t *, uint32_t *))
	    __wt_db_lockout;
	db->btree_pagesize_set = (int (*)
	    (DB *, uint32_t , uint32_t , uint32_t , uint32_t , uint32_t ))
	    __wt_db_lockout;
	db->bulk_load = (int (*)
	    (DB *, uint32_t , void (*)(const char *, uint64_t), int (*)(DB *, DBT **, DBT **)))
	    __wt_db_lockout;
	db->col_del = (int (*)
	    (DB *, WT_TOC *, uint64_t , uint32_t ))
	    __wt_db_lockout;
	db->col_get = (int (*)
	    (DB *, WT_TOC *, uint64_t , DBT *, uint32_t ))
	    __wt_db_lockout;
	db->col_put = (int (*)
	    (DB *, WT_TOC *, uint64_t , DBT *, uint32_t ))
	    __wt_db_lockout;
	db->column_set = (int (*)
	    (DB *, uint32_t , const char *, uint32_t ))
	    __wt_db_lockout;
	db->dump = (int (*)
	    (DB *, FILE *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_db_lockout;
	db->err = (void (*)
	    (DB *, int , const char *, ...))
	    __wt_db_lockout;
	db->errcall_get = (int (*)
	    (DB *, void (**)(const DB *, const char *)))
	    __wt_db_lockout;
	db->errcall_set = (int (*)
	    (DB *, void (*)(const DB *, const char *)))
	    __wt_db_lockout;
	db->errfile_get = (int (*)
	    (DB *, FILE **))
	    __wt_db_lockout;
	db->errfile_set = (int (*)
	    (DB *, FILE *))
	    __wt_db_lockout;
	db->errpfx_get = (int (*)
	    (DB *, const char **))
	    __wt_db_lockout;
	db->errpfx_set = (int (*)
	    (DB *, const char *))
	    __wt_db_lockout;
	db->errx = (void (*)
	    (DB *, const char *, ...))
	    __wt_db_lockout;
	db->huffman_set = (int (*)
	    (DB *, uint8_t const *, u_int , uint32_t ))
	    __wt_db_lockout;
	db->open = (int (*)
	    (DB *, const char *, mode_t , uint32_t ))
	    __wt_db_lockout;
	db->row_del = (int (*)
	    (DB *, WT_TOC *, DBT *, uint32_t ))
	    __wt_db_lockout;
	db->row_get = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, uint32_t ))
	    __wt_db_lockout;
	db->row_put = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, uint32_t ))
	    __wt_db_lockout;
	db->stat_clear = (int (*)
	    (DB *, uint32_t ))
	    __wt_db_lockout;
	db->stat_print = (int (*)
	    (DB *, FILE *, uint32_t ))
	    __wt_db_lockout;
	db->sync = (int (*)
	    (DB *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_db_lockout;
	db->verify = (int (*)
	    (DB *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_db_lockout;
}

void
__wt_methods_db_init_transition(DB *db)
{
	db->btree_compare_dup_get = __wt_api_db_btree_compare_dup_get;
	db->btree_compare_dup_set = __wt_api_db_btree_compare_dup_set;
	db->btree_compare_get = __wt_api_db_btree_compare_get;
	db->btree_compare_int_get = __wt_api_db_btree_compare_int_get;
	db->btree_compare_int_set = __wt_api_db_btree_compare_int_set;
	db->btree_compare_set = __wt_api_db_btree_compare_set;
	db->btree_dup_offpage_get = __wt_api_db_btree_dup_offpage_get;
	db->btree_dup_offpage_set = __wt_api_db_btree_dup_offpage_set;
	db->btree_itemsize_get = __wt_api_db_btree_itemsize_get;
	db->btree_itemsize_set = __wt_api_db_btree_itemsize_set;
	db->btree_pagesize_get = __wt_api_db_btree_pagesize_get;
	db->btree_pagesize_set = __wt_api_db_btree_pagesize_set;
	db->close = __wt_api_db_close;
	db->column_set = __wt_api_db_column_set;
	db->err = __wt_api_db_err;
	db->errcall_get = __wt_api_db_errcall_get;
	db->errcall_set = __wt_api_db_errcall_set;
	db->errfile_get = __wt_api_db_errfile_get;
	db->errfile_set = __wt_api_db_errfile_set;
	db->errpfx_get = __wt_api_db_errpfx_get;
	db->errpfx_set = __wt_api_db_errpfx_set;
	db->errx = __wt_api_db_errx;
	db->huffman_set = __wt_api_db_huffman_set;
	db->open = __wt_api_db_open;
}

void
__wt_methods_db_open_transition(DB *db)
{
	db->btree_compare_dup_set = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout;
	db->btree_compare_int_set = (int (*)
	    (DB *, int ))
	    __wt_db_lockout;
	db->btree_compare_set = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout;
	db->btree_dup_offpage_set = (int (*)
	    (DB *, uint32_t ))
	    __wt_db_lockout;
	db->btree_itemsize_set = (int (*)
	    (DB *, uint32_t , uint32_t ))
	    __wt_db_lockout;
	db->btree_pagesize_set = (int (*)
	    (DB *, uint32_t , uint32_t , uint32_t , uint32_t , uint32_t ))
	    __wt_db_lockout;
	db->column_set = (int (*)
	    (DB *, uint32_t , const char *, uint32_t ))
	    __wt_db_lockout;
	db->huffman_set = (int (*)
	    (DB *, uint8_t const *, u_int , uint32_t ))
	    __wt_db_lockout;
	db->bulk_load = __wt_api_db_bulk_load;
	db->col_del = __wt_api_db_col_del;
	db->col_get = __wt_api_db_col_get;
	db->col_put = __wt_api_db_col_put;
	db->dump = __wt_api_db_dump;
	db->row_del = __wt_api_db_row_del;
	db->row_get = __wt_api_db_row_get;
	db->row_put = __wt_api_db_row_put;
	db->stat_clear = __wt_api_db_stat_clear;
	db->stat_print = __wt_api_db_stat_print;
	db->sync = __wt_api_db_sync;
	db->verify = __wt_api_db_verify;
}

void
__wt_methods_env_config_default(ENV *env)
{
	env->cache_drain_cnt = 10;
	env->cache_size = 20;
	env->data_update_initial = 8 * 1024;
	env->data_update_max = 32 * 1024;
	env->hazard_size = 15;
	env->toc_size = 50;
}

void
__wt_methods_env_lockout(ENV *env)
{
	env->cache_drain_cnt_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->cache_drain_cnt_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->cache_hash_size_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->cache_hash_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->cache_size_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->cache_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->data_update_initial_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->data_update_initial_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->data_update_max_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->data_update_max_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->db = (int (*)
	    (ENV *, uint32_t , DB **))
	    __wt_env_lockout;
	env->err = (void (*)
	    (ENV *, int , const char *, ...))
	    __wt_env_lockout;
	env->errcall_get = (int (*)
	    (ENV *, void (**)(const ENV *, const char *)))
	    __wt_env_lockout;
	env->errcall_set = (int (*)
	    (ENV *, void (*)(const ENV *, const char *)))
	    __wt_env_lockout;
	env->errfile_get = (int (*)
	    (ENV *, FILE **))
	    __wt_env_lockout;
	env->errfile_set = (int (*)
	    (ENV *, FILE *))
	    __wt_env_lockout;
	env->errpfx_get = (int (*)
	    (ENV *, const char **))
	    __wt_env_lockout;
	env->errpfx_set = (int (*)
	    (ENV *, const char *))
	    __wt_env_lockout;
	env->errx = (void (*)
	    (ENV *, const char *, ...))
	    __wt_env_lockout;
	env->hazard_size_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->hazard_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->msgcall_get = (int (*)
	    (ENV *, void (**)(const ENV *, const char *)))
	    __wt_env_lockout;
	env->msgcall_set = (int (*)
	    (ENV *, void (*)(const ENV *, const char *)))
	    __wt_env_lockout;
	env->msgfile_get = (int (*)
	    (ENV *, FILE **))
	    __wt_env_lockout;
	env->msgfile_set = (int (*)
	    (ENV *, FILE *))
	    __wt_env_lockout;
	env->open = (int (*)
	    (ENV *, const char *, mode_t , uint32_t ))
	    __wt_env_lockout;
	env->stat_clear = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->stat_print = (int (*)
	    (ENV *, FILE *, uint32_t ))
	    __wt_env_lockout;
	env->sync = (int (*)
	    (ENV *, void (*)(const char *, uint64_t), uint32_t ))
	    __wt_env_lockout;
	env->toc = (int (*)
	    (ENV *, uint32_t , WT_TOC **))
	    __wt_env_lockout;
	env->toc_size_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->toc_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->verbose_get = (int (*)
	    (ENV *, uint32_t *))
	    __wt_env_lockout;
	env->verbose_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
}

void
__wt_methods_env_init_transition(ENV *env)
{
	env->cache_drain_cnt_get = __wt_api_env_cache_drain_cnt_get;
	env->cache_drain_cnt_set = __wt_api_env_cache_drain_cnt_set;
	env->cache_hash_size_get = __wt_api_env_cache_hash_size_get;
	env->cache_hash_size_set = __wt_api_env_cache_hash_size_set;
	env->cache_size_get = __wt_api_env_cache_size_get;
	env->cache_size_set = __wt_api_env_cache_size_set;
	env->close = __wt_api_env_close;
	env->data_update_initial_get = __wt_api_env_data_update_initial_get;
	env->data_update_initial_set = __wt_api_env_data_update_initial_set;
	env->data_update_max_get = __wt_api_env_data_update_max_get;
	env->data_update_max_set = __wt_api_env_data_update_max_set;
	env->err = __wt_api_env_err;
	env->errcall_get = __wt_api_env_errcall_get;
	env->errcall_set = __wt_api_env_errcall_set;
	env->errfile_get = __wt_api_env_errfile_get;
	env->errfile_set = __wt_api_env_errfile_set;
	env->errpfx_get = __wt_api_env_errpfx_get;
	env->errpfx_set = __wt_api_env_errpfx_set;
	env->errx = __wt_api_env_errx;
	env->hazard_size_get = __wt_api_env_hazard_size_get;
	env->hazard_size_set = __wt_api_env_hazard_size_set;
	env->msgcall_get = __wt_api_env_msgcall_get;
	env->msgcall_set = __wt_api_env_msgcall_set;
	env->msgfile_get = __wt_api_env_msgfile_get;
	env->msgfile_set = __wt_api_env_msgfile_set;
	env->open = __wt_api_env_open;
	env->stat_clear = __wt_api_env_stat_clear;
	env->stat_print = __wt_api_env_stat_print;
	env->toc_size_get = __wt_api_env_toc_size_get;
	env->toc_size_set = __wt_api_env_toc_size_set;
	env->verbose_get = __wt_api_env_verbose_get;
	env->verbose_set = __wt_api_env_verbose_set;
}

void
__wt_methods_env_open_transition(ENV *env)
{
	env->cache_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->hazard_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->open = (int (*)
	    (ENV *, const char *, mode_t , uint32_t ))
	    __wt_env_lockout;
	env->toc_size_set = (int (*)
	    (ENV *, uint32_t ))
	    __wt_env_lockout;
	env->db = __wt_api_env_db;
	env->sync = __wt_api_env_sync;
	env->toc = __wt_api_env_toc;
}

void
__wt_methods_wt_toc_lockout(WT_TOC *wt_toc)
{
	WT_CC_QUIET(wt_toc, NULL);
}

void
__wt_methods_wt_toc_init_transition(WT_TOC *wt_toc)
{
	wt_toc->close = __wt_api_wt_toc_close;
}

