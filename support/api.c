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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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

	WT_RET((__wt_db_btree_compare_int_set_verify(db, btree_compare_int)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_COMPARE_INT_SET);
	db->btree_compare_int = btree_compare_int;
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_dup_offpage_get(
	DB *db,
	u_int32_t *btree_dup_offpage);
static int __wt_api_db_btree_dup_offpage_get(
	DB *db,
	u_int32_t *btree_dup_offpage)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_DUP_OFFPAGE_GET);
	*btree_dup_offpage = db->btree_dup_offpage;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_dup_offpage_set(
	DB *db,
	u_int32_t btree_dup_offpage);
static int __wt_api_db_btree_dup_offpage_set(
	DB *db,
	u_int32_t btree_dup_offpage)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	WT_RET((__wt_db_btree_dup_offpage_set_verify(db, btree_dup_offpage)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_DUP_OFFPAGE_SET);
	db->btree_dup_offpage = btree_dup_offpage;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_itemsize_get(
	DB *db,
	u_int32_t *intlitemsize,
	u_int32_t *leafitemsize);
static int __wt_api_db_btree_itemsize_get(
	DB *db,
	u_int32_t *intlitemsize,
	u_int32_t *leafitemsize)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_ITEMSIZE_GET);
	*intlitemsize = db->intlitemsize;
	*leafitemsize = db->leafitemsize;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_itemsize_set(
	DB *db,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize);
static int __wt_api_db_btree_itemsize_set(
	DB *db,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_ITEMSIZE_SET);
	db->intlitemsize = intlitemsize;
	db->leafitemsize = leafitemsize;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_pagesize_get(
	DB *db,
	u_int32_t *allocsize,
	u_int32_t *intlsize,
	u_int32_t *leafsize,
	u_int32_t *extsize);
static int __wt_api_db_btree_pagesize_get(
	DB *db,
	u_int32_t *allocsize,
	u_int32_t *intlsize,
	u_int32_t *leafsize,
	u_int32_t *extsize)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_PAGESIZE_GET);
	*allocsize = db->allocsize;
	*intlsize = db->intlsize;
	*leafsize = db->leafsize;
	*extsize = db->extsize;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_btree_pagesize_set(
	DB *db,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize);
static int __wt_api_db_btree_pagesize_set(
	DB *db,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_BTREE_PAGESIZE_SET);
	db->allocsize = allocsize;
	db->intlsize = intlsize;
	db->leafsize = leafsize;
	db->extsize = extsize;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_bulk_load(
	DB *db,
	u_int32_t flags,
	void (*progress)(const char *, u_int64_t),
	int (*cb)(DB *, DBT **, DBT **));
static int __wt_api_db_bulk_load(
	DB *db,
	u_int32_t flags,
	void (*progress)(const char *, u_int64_t),
	int (*cb)(DB *, DBT **, DBT **))
{
	const char *method_name = "DB.bulk_load";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_BULK_LOAD);
	WT_DB_RDONLY(db, method_name);
	WT_STAT_INCR(ienv->method_stats, DB_BULK_LOAD);
	ret = __wt_db_bulk_load(db, flags, progress, cb);
	return (ret);
}

static int __wt_api_db_close(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_close(
	DB *db,
	u_int32_t flags)
{
	const char *method_name = "DB.close";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_CLOSE);
	WT_STAT_INCR(ienv->method_stats, DB_CLOSE);
	ret = __wt_db_close(db);
	return (ret);
}

static int __wt_api_db_column_set(
	DB *db,
	u_int32_t fixed_len,
	const char *dictionary,
	u_int32_t flags);
static int __wt_api_db_column_set(
	DB *db,
	u_int32_t fixed_len,
	const char *dictionary,
	u_int32_t flags)
{
	ENV *env = db->env;
	IENV *ienv = env->ienv;

	WT_ENV_FCHK(env, "DB.column_set",
	    flags, WT_APIMASK_DB_COLUMN_SET);

	WT_RET((__wt_db_column_set_verify(db, fixed_len, dictionary, flags)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, DB_COLUMN_SET);
	db->fixed_len = fixed_len;
	db->dictionary = dictionary;
	db->flags = flags;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_db_del(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	u_int32_t flags);
static int __wt_api_db_del(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	u_int32_t flags)
{
	const char *method_name = "DB.del";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_DEL);
	WT_DB_RDONLY(db, method_name);
	WT_TOC_SET_GEN(toc);
	WT_STAT_INCR(ienv->method_stats, DB_DEL);
	while ((ret = __wt_db_del(db, toc, key)) == WT_RESTART)
		;
	WT_TOC_CLR_GEN(toc);
	return (ret);
}

static int __wt_api_db_dump(
	DB *db,
	FILE *stream,
	void (*progress)(const char *, u_int64_t),
	u_int32_t flags);
static int __wt_api_db_dump(
	DB *db,
	FILE *stream,
	void (*progress)(const char *, u_int64_t),
	u_int32_t flags)
{
	const char *method_name = "DB.dump";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_DUMP);
	WT_STAT_INCR(ienv->method_stats, DB_DUMP);
	ret = __wt_db_dump(db, stream, progress, flags);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
	return (0);
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
	const char *method_name = "DB.get";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_GET);
	WT_TOC_SET_GEN(toc);
	WT_STAT_INCR(ienv->method_stats, DB_GET);
	ret = __wt_db_get(db, toc, key, pkey, data);
	WT_TOC_CLR_GEN(toc);
	return (ret);
}

static int __wt_api_db_get_recno(
	DB *db,
	WT_TOC *toc,
	u_int64_t recno,
	DBT *data,
	u_int32_t flags);
static int __wt_api_db_get_recno(
	DB *db,
	WT_TOC *toc,
	u_int64_t recno,
	DBT *data,
	u_int32_t flags)
{
	const char *method_name = "DB.get_recno";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_GET_RECNO);
	WT_TOC_SET_GEN(toc);
	WT_STAT_INCR(ienv->method_stats, DB_GET_RECNO);
	ret = __wt_db_get_recno(db, toc, recno, data);
	WT_TOC_CLR_GEN(toc);
	return (ret);
}

static int __wt_api_db_huffman_set(
	DB *db,
	u_int8_t const *huffman_table,
	u_int huffman_table_size,
	u_int32_t huffman_flags);
static int __wt_api_db_huffman_set(
	DB *db,
	u_int8_t const *huffman_table,
	u_int huffman_table_size,
	u_int32_t huffman_flags)
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
	__wt_unlock(ienv->mtx);
	return (ret);
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
	const char *method_name = "DB.open";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_OPEN);
	WT_STAT_INCR(ienv->method_stats, DB_OPEN);
	ret = __wt_db_open(db, dbname, mode, flags);
	return (ret);
}

static int __wt_api_db_put(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *data,
	u_int32_t flags);
static int __wt_api_db_put(
	DB *db,
	WT_TOC *toc,
	DBT *key,
	DBT *data,
	u_int32_t flags)
{
	const char *method_name = "DB.put";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_PUT);
	WT_DB_RDONLY(db, method_name);
	WT_TOC_SET_GEN(toc);
	WT_STAT_INCR(ienv->method_stats, DB_PUT);
	while ((ret = __wt_db_put(db, toc, key, data)) == WT_RESTART)
		;
	WT_TOC_CLR_GEN(toc);
	return (ret);
}

static int __wt_api_db_stat_clear(
	DB *db,
	u_int32_t flags);
static int __wt_api_db_stat_clear(
	DB *db,
	u_int32_t flags)
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
	u_int32_t flags);
static int __wt_api_db_stat_print(
	DB *db,
	FILE *stream,
	u_int32_t flags)
{
	const char *method_name = "DB.stat_print";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_STAT_PRINT);
	WT_STAT_INCR(ienv->method_stats, DB_STAT_PRINT);
	ret = __wt_db_stat_print(db, stream);
	return (ret);
}

static int __wt_api_db_sync(
	DB *db,
	void (*progress)(const char *, u_int64_t),
	u_int32_t flags);
static int __wt_api_db_sync(
	DB *db,
	void (*progress)(const char *, u_int64_t),
	u_int32_t flags)
{
	const char *method_name = "DB.sync";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_SYNC);
	WT_DB_RDONLY(db, method_name);
	WT_STAT_INCR(ienv->method_stats, DB_SYNC);
	ret = __wt_db_sync(db, progress);
	return (ret);
}

static int __wt_api_db_verify(
	DB *db,
	void (*progress)(const char *, u_int64_t),
	u_int32_t flags);
static int __wt_api_db_verify(
	DB *db,
	void (*progress)(const char *, u_int64_t),
	u_int32_t flags)
{
	const char *method_name = "DB.verify";
	ENV *env = db->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_DB_VERIFY);
	WT_STAT_INCR(ienv->method_stats, DB_VERIFY);
	ret = __wt_db_verify(db, progress);
	return (ret);
}

static int __wt_api_env_cache_hash_size_get(
	ENV *env,
	u_int32_t *cache_hash_size);
static int __wt_api_env_cache_hash_size_get(
	ENV *env,
	u_int32_t *cache_hash_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_HASH_SIZE_GET);
	*cache_hash_size = env->cache_hash_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_hash_size_set(
	ENV *env,
	u_int32_t cache_hash_size);
static int __wt_api_env_cache_hash_size_set(
	ENV *env,
	u_int32_t cache_hash_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_cache_hash_size_set_verify(env, cache_hash_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_HASH_SIZE_SET);
	env->cache_hash_size = cache_hash_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_size_get(
	ENV *env,
	u_int32_t *cache_size);
static int __wt_api_env_cache_size_get(
	ENV *env,
	u_int32_t *cache_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_SIZE_GET);
	*cache_size = env->cache_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_cache_size_set(
	ENV *env,
	u_int32_t cache_size);
static int __wt_api_env_cache_size_set(
	ENV *env,
	u_int32_t cache_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_cache_size_set_verify(env, cache_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_CACHE_SIZE_SET);
	env->cache_size = cache_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_close(
	ENV *env,
	u_int32_t flags);
static int __wt_api_env_close(
	ENV *env,
	u_int32_t flags)
{
	const char *method_name = "ENV.close";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_CLOSE);
	WT_STAT_INCR(ienv->method_stats, ENV_CLOSE);
	ret = __wt_env_close(env);
	return (ret);
}

static int __wt_api_env_db(
	ENV *env,
	u_int32_t flags,
	DB **dbp);
static int __wt_api_env_db(
	ENV *env,
	u_int32_t flags,
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_hazard_size_get(
	ENV *env,
	u_int32_t *hazard_size);
static int __wt_api_env_hazard_size_get(
	ENV *env,
	u_int32_t *hazard_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_HAZARD_SIZE_GET);
	*hazard_size = env->hazard_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_hazard_size_set(
	ENV *env,
	u_int32_t hazard_size);
static int __wt_api_env_hazard_size_set(
	ENV *env,
	u_int32_t hazard_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_hazard_size_set_verify(env, hazard_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_HAZARD_SIZE_SET);
	env->hazard_size = hazard_size;
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
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
	__wt_unlock(ienv->mtx);
	return (0);
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
	u_int32_t flags);
static int __wt_api_env_stat_clear(
	ENV *env,
	u_int32_t flags)
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
	u_int32_t flags);
static int __wt_api_env_stat_print(
	ENV *env,
	FILE *stream,
	u_int32_t flags)
{
	const char *method_name = "ENV.stat_print";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_STAT_PRINT);
	WT_STAT_INCR(ienv->method_stats, ENV_STAT_PRINT);
	ret = __wt_env_stat_print(env, stream);
	return (ret);
}

static int __wt_api_env_toc(
	ENV *env,
	u_int32_t flags,
	WT_TOC **tocp);
static int __wt_api_env_toc(
	ENV *env,
	u_int32_t flags,
	WT_TOC **tocp)
{
	const char *method_name = "ENV.toc";
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_ENV_TOC);
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_TOC);
	ret = __wt_env_toc(env, tocp);
	__wt_unlock(ienv->mtx);
	return (ret);
}

static int __wt_api_env_toc_size_get(
	ENV *env,
	u_int32_t *toc_size);
static int __wt_api_env_toc_size_get(
	ENV *env,
	u_int32_t *toc_size)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_TOC_SIZE_GET);
	*toc_size = env->toc_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_toc_size_set(
	ENV *env,
	u_int32_t toc_size);
static int __wt_api_env_toc_size_set(
	ENV *env,
	u_int32_t toc_size)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_toc_size_set_verify(env, toc_size)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_TOC_SIZE_SET);
	env->toc_size = toc_size;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_verbose_get(
	ENV *env,
	u_int32_t *verbose);
static int __wt_api_env_verbose_get(
	ENV *env,
	u_int32_t *verbose)
{
	IENV *ienv = env->ienv;
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_VERBOSE_GET);
	*verbose = env->verbose;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_env_verbose_set(
	ENV *env,
	u_int32_t verbose);
static int __wt_api_env_verbose_set(
	ENV *env,
	u_int32_t verbose)
{
	IENV *ienv = env->ienv;
	WT_RET((__wt_env_verbose_set_verify(env, verbose)));
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, ENV_VERBOSE_SET);
	env->verbose = verbose;
	__wt_unlock(ienv->mtx);
	return (0);
}

static int __wt_api_wt_toc_close(
	WT_TOC *wt_toc,
	u_int32_t flags);
static int __wt_api_wt_toc_close(
	WT_TOC *wt_toc,
	u_int32_t flags)
{
	const char *method_name = "WT_TOC.close";
	ENV *env = wt_toc->env;
	IENV *ienv = env->ienv;
	int ret;

	WT_ENV_FCHK(env, method_name, flags, WT_APIMASK_WT_TOC_CLOSE);
	__wt_lock(env, ienv->mtx);
	WT_STAT_INCR(ienv->method_stats, WT_TOC_CLOSE);
	ret = __wt_wt_toc_close(wt_toc);
	__wt_unlock(ienv->mtx);
	return (ret);
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
	    (DB *, u_int32_t *))
	    __wt_db_lockout;
	db->btree_dup_offpage_set = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout;
	db->btree_itemsize_get = (int (*)
	    (DB *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout;
	db->btree_itemsize_set = (int (*)
	    (DB *, u_int32_t , u_int32_t ))
	    __wt_db_lockout;
	db->btree_pagesize_get = (int (*)
	    (DB *, u_int32_t *, u_int32_t *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout;
	db->btree_pagesize_set = (int (*)
	    (DB *, u_int32_t , u_int32_t , u_int32_t , u_int32_t ))
	    __wt_db_lockout;
	db->bulk_load = (int (*)
	    (DB *, u_int32_t , void (*)(const char *, u_int64_t), int (*)(DB *, DBT **, DBT **)))
	    __wt_db_lockout;
	db->column_set = (int (*)
	    (DB *, u_int32_t , const char *, u_int32_t ))
	    __wt_db_lockout;
	db->del = (int (*)
	    (DB *, WT_TOC *, DBT *, u_int32_t ))
	    __wt_db_lockout;
	db->dump = (int (*)
	    (DB *, FILE *, void (*)(const char *, u_int64_t), u_int32_t ))
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
	db->get = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout;
	db->get_recno = (int (*)
	    (DB *, WT_TOC *, u_int64_t , DBT *, u_int32_t ))
	    __wt_db_lockout;
	db->huffman_set = (int (*)
	    (DB *, u_int8_t const *, u_int , u_int32_t ))
	    __wt_db_lockout;
	db->open = (int (*)
	    (DB *, const char *, mode_t , u_int32_t ))
	    __wt_db_lockout;
	db->put = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout;
	db->stat_clear = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout;
	db->stat_print = (int (*)
	    (DB *, FILE *, u_int32_t ))
	    __wt_db_lockout;
	db->sync = (int (*)
	    (DB *, void (*)(const char *, u_int64_t), u_int32_t ))
	    __wt_db_lockout;
	db->verify = (int (*)
	    (DB *, void (*)(const char *, u_int64_t), u_int32_t ))
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
	    (DB *, u_int32_t ))
	    __wt_db_lockout;
	db->btree_itemsize_set = (int (*)
	    (DB *, u_int32_t , u_int32_t ))
	    __wt_db_lockout;
	db->btree_pagesize_set = (int (*)
	    (DB *, u_int32_t , u_int32_t , u_int32_t , u_int32_t ))
	    __wt_db_lockout;
	db->column_set = (int (*)
	    (DB *, u_int32_t , const char *, u_int32_t ))
	    __wt_db_lockout;
	db->huffman_set = (int (*)
	    (DB *, u_int8_t const *, u_int , u_int32_t ))
	    __wt_db_lockout;
	db->bulk_load = __wt_api_db_bulk_load;
	db->del = __wt_api_db_del;
	db->dump = __wt_api_db_dump;
	db->get = __wt_api_db_get;
	db->get_recno = __wt_api_db_get_recno;
	db->put = __wt_api_db_put;
	db->stat_clear = __wt_api_db_stat_clear;
	db->stat_print = __wt_api_db_stat_print;
	db->sync = __wt_api_db_sync;
	db->verify = __wt_api_db_verify;
}

void
__wt_methods_env_lockout(ENV *env)
{
	env->cache_hash_size_get = (int (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout;
	env->cache_hash_size_set = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->cache_size_get = (int (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout;
	env->cache_size_set = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->db = (int (*)
	    (ENV *, u_int32_t , DB **))
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
	    (ENV *, u_int32_t *))
	    __wt_env_lockout;
	env->hazard_size_set = (int (*)
	    (ENV *, u_int32_t ))
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
	    (ENV *, const char *, mode_t , u_int32_t ))
	    __wt_env_lockout;
	env->stat_clear = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->stat_print = (int (*)
	    (ENV *, FILE *, u_int32_t ))
	    __wt_env_lockout;
	env->toc = (int (*)
	    (ENV *, u_int32_t , WT_TOC **))
	    __wt_env_lockout;
	env->toc_size_get = (int (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout;
	env->toc_size_set = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->verbose_get = (int (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout;
	env->verbose_set = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
}

void
__wt_methods_env_init_transition(ENV *env)
{
	env->cache_hash_size_get = __wt_api_env_cache_hash_size_get;
	env->cache_hash_size_set = __wt_api_env_cache_hash_size_set;
	env->cache_size_get = __wt_api_env_cache_size_get;
	env->cache_size_set = __wt_api_env_cache_size_set;
	env->close = __wt_api_env_close;
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
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->hazard_size_set = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->open = (int (*)
	    (ENV *, const char *, mode_t , u_int32_t ))
	    __wt_env_lockout;
	env->toc_size_set = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout;
	env->db = __wt_api_env_db;
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

