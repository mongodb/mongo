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

	__wt_lock(env, &env->ienv->mtx);
	*(btree_compare_dup) = db->btree_compare_dup;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->btree_compare_dup = btree_compare_dup;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(btree_compare) = db->btree_compare;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(btree_compare_int) = db->btree_compare_int;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	WT_RET((__wt_db_btree_compare_int_set_verify(db, btree_compare_int)));
	db->btree_compare_int = btree_compare_int;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->btree_compare = btree_compare;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(btree_dup_offpage) = db->btree_dup_offpage;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->btree_dup_offpage = btree_dup_offpage;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(intlitemsize) = db->intlitemsize;
	*(leafitemsize) = db->leafitemsize;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->intlitemsize = intlitemsize;
	db->leafitemsize = leafitemsize;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(allocsize) = db->allocsize;
	*(intlsize) = db->intlsize;
	*(leafsize) = db->leafsize;
	*(extsize) = db->extsize;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->allocsize = allocsize;
	db->intlsize = intlsize;
	db->leafsize = leafsize;
	db->extsize = extsize;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_db_errcall_get(
	DB *db,
	void (**errcall)(const DB *, const char *));
static int __wt_api_db_errcall_get(
	DB *db,
	void (**errcall)(const DB *, const char *))
{
	ENV *env = db->env;

	__wt_lock(env, &env->ienv->mtx);
	*(errcall) = db->errcall;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->errcall = errcall;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(errfile) = db->errfile;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->errfile = errfile;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	*(errpfx) = db->errpfx;
	__wt_unlock(&env->ienv->mtx);
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

	__wt_lock(env, &env->ienv->mtx);
	db->errpfx = errpfx;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_cachesize_get(
	ENV *env,
	u_int32_t *cachesize);
static int __wt_api_env_cachesize_get(
	ENV *env,
	u_int32_t *cachesize)
{
	__wt_lock(env, &env->ienv->mtx);
	*(cachesize) = env->cachesize;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_cachesize_set(
	ENV *env,
	u_int32_t cachesize);
static int __wt_api_env_cachesize_set(
	ENV *env,
	u_int32_t cachesize)
{
	__wt_lock(env, &env->ienv->mtx);
	env->cachesize = cachesize;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_errcall_get(
	ENV *env,
	void (**errcall)(const ENV *, const char *));
static int __wt_api_env_errcall_get(
	ENV *env,
	void (**errcall)(const ENV *, const char *))
{
	__wt_lock(env, &env->ienv->mtx);
	*(errcall) = env->errcall;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_errcall_set(
	ENV *env,
	void (*errcall)(const ENV *, const char *));
static int __wt_api_env_errcall_set(
	ENV *env,
	void (*errcall)(const ENV *, const char *))
{
	__wt_lock(env, &env->ienv->mtx);
	env->errcall = errcall;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_errfile_get(
	ENV *env,
	FILE **errfile);
static int __wt_api_env_errfile_get(
	ENV *env,
	FILE **errfile)
{
	__wt_lock(env, &env->ienv->mtx);
	*(errfile) = env->errfile;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_errfile_set(
	ENV *env,
	FILE *errfile);
static int __wt_api_env_errfile_set(
	ENV *env,
	FILE *errfile)
{
	__wt_lock(env, &env->ienv->mtx);
	env->errfile = errfile;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_errpfx_get(
	ENV *env,
	const char **errpfx);
static int __wt_api_env_errpfx_get(
	ENV *env,
	const char **errpfx)
{
	__wt_lock(env, &env->ienv->mtx);
	*(errpfx) = env->errpfx;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_errpfx_set(
	ENV *env,
	const char *errpfx);
static int __wt_api_env_errpfx_set(
	ENV *env,
	const char *errpfx)
{
	__wt_lock(env, &env->ienv->mtx);
	env->errpfx = errpfx;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_verbose_get(
	ENV *env,
	u_int32_t *verbose);
static int __wt_api_env_verbose_get(
	ENV *env,
	u_int32_t *verbose)
{
	__wt_lock(env, &env->ienv->mtx);
	*(verbose) = env->verbose;
	__wt_unlock(&env->ienv->mtx);
	return (0);
}

static int __wt_api_env_verbose_set(
	ENV *env,
	u_int32_t verbose);
static int __wt_api_env_verbose_set(
	ENV *env,
	u_int32_t verbose)
{
	__wt_lock(env, &env->ienv->mtx);
	WT_RET((__wt_env_verbose_set_verify(env, verbose)));
	env->verbose = verbose;
	__wt_unlock(&env->ienv->mtx);
	return (0);
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
	    (DB *, u_int32_t , int (*)(DB *, DBT **, DBT **)))
	    __wt_db_lockout;
	db->dump = (int (*)
	    (DB *, FILE *, u_int32_t ))
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
	db->get = (int (*)
	    (DB *, WT_TOC *, DBT *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout;
	db->get_recno = (int (*)
	    (DB *, WT_TOC *, u_int64_t , DBT *, DBT *, DBT *, u_int32_t ))
	    __wt_db_lockout;
	db->open = (int (*)
	    (DB *, const char *, mode_t , u_int32_t ))
	    __wt_db_lockout;
	db->stat_clear = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout;
	db->stat_print = (int (*)
	    (DB *, FILE * , u_int32_t ))
	    __wt_db_lockout;
	db->sync = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout;
	db->verify = (int (*)
	    (DB *, u_int32_t ))
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
	db->errcall_get = __wt_api_db_errcall_get;
	db->errcall_set = __wt_api_db_errcall_set;
	db->errfile_get = __wt_api_db_errfile_get;
	db->errfile_set = __wt_api_db_errfile_set;
	db->errpfx_get = __wt_api_db_errpfx_get;
	db->errpfx_set = __wt_api_db_errpfx_set;
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
	db->bulk_load = __wt_api_db_bulk_load;
	db->dump = __wt_api_db_dump;
	db->get = __wt_api_db_get;
	db->get_recno = __wt_api_db_get_recno;
	db->stat_clear = __wt_api_db_stat_clear;
	db->stat_print = __wt_api_db_stat_print;
	db->sync = __wt_api_db_sync;
	db->verify = __wt_api_db_verify;
}

void
__wt_methods_env_lockout(ENV *env)
{
	env->cachesize_get = (int (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout;
	env->cachesize_set = (int (*)
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
	env->cachesize_get = __wt_api_env_cachesize_get;
	env->cachesize_set = __wt_api_env_cachesize_set;
	env->close = __wt_api_env_close;
	env->err = __wt_api_env_err;
	env->errcall_get = __wt_api_env_errcall_get;
	env->errcall_set = __wt_api_env_errcall_set;
	env->errfile_get = __wt_api_env_errfile_get;
	env->errfile_set = __wt_api_env_errfile_set;
	env->errpfx_get = __wt_api_env_errpfx_get;
	env->errpfx_set = __wt_api_env_errpfx_set;
	env->errx = __wt_api_env_errx;
	env->open = __wt_api_env_open;
	env->stat_clear = __wt_api_env_stat_clear;
	env->stat_print = __wt_api_env_stat_print;
	env->verbose_get = __wt_api_env_verbose_get;
	env->verbose_set = __wt_api_env_verbose_set;
}

void
__wt_methods_env_open_transition(ENV *env)
{
	env->open = (int (*)
	    (ENV *, const char *, mode_t , u_int32_t ))
	    __wt_env_lockout;
	env->db = __wt_api_env_db;
	env->toc = __wt_api_env_toc;
}

void
__wt_methods_wt_toc_lockout(WT_TOC *wt_toc)
{
}

void
__wt_methods_wt_toc_init_transition(WT_TOC *wt_toc)
{
	wt_toc->close = __wt_api_wt_toc_close;
}

