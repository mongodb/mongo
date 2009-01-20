/* DO NOT EDIT: automatically built by dist/api.py. */

#include "wt_internal.h"

static void __wt_env_get_errcall(
	ENV *,
	void (**)(const ENV *, const char *));
static void
__wt_env_get_errcall(
	ENV *handle,
	void (**errcallp)(const ENV *, const char *))
{
	*errcallp = handle->errcall;
}

static int __wt_env_set_errcall(
	ENV *,
	void (*)(const ENV *, const char *));
static int
__wt_env_set_errcall(
	ENV *handle,
	void (*errcall)(const ENV *, const char *))
{
	handle->errcall = errcall;
	return (0);
}

static void __wt_env_get_errfile(
	ENV *,
	FILE **);
static void
__wt_env_get_errfile(
	ENV *handle,
	FILE **errfilep)
{
	*errfilep = handle->errfile;
}

static int __wt_env_set_errfile(
	ENV *,
	FILE *);
static int
__wt_env_set_errfile(
	ENV *handle,
	FILE *errfile)
{
	handle->errfile = errfile;
	return (0);
}

static void __wt_env_get_errpfx(
	ENV *,
	const char **);
static void
__wt_env_get_errpfx(
	ENV *handle,
	const char **errpfxp)
{
	*errpfxp = handle->errpfx;
}

static int __wt_env_set_errpfx(
	ENV *,
	const char *);
static int
__wt_env_set_errpfx(
	ENV *handle,
	const char *errpfx)
{
	handle->errpfx = errpfx;
	return (0);
}

static void __wt_env_get_verbose(
	ENV *,
	u_int32_t *);
static void
__wt_env_get_verbose(
	ENV *handle,
	u_int32_t *verbosep)
{
	*verbosep = handle->verbose;
}

static int __wt_env_set_verbose(
	ENV *,
	u_int32_t );
static int
__wt_env_set_verbose(
	ENV *handle,
	u_int32_t verbose)
{
	int ret;

	if ((ret = __wt_env_set_verbose_verify(
	    handle, &verbose)) != 0)
		return (ret);

	handle->verbose = verbose;
	return (0);
}

static void __wt_env_get_cachesize(
	ENV *,
	u_int32_t *);
static void
__wt_env_get_cachesize(
	ENV *handle,
	u_int32_t *cachesizep)
{
	*cachesizep = handle->cachesize;
}

static int __wt_env_set_cachesize(
	ENV *,
	u_int32_t );
static int
__wt_env_set_cachesize(
	ENV *handle,
	u_int32_t cachesize)
{
	handle->cachesize = cachesize;
	return (0);
}

static void __wt_db_get_errcall(
	DB *,
	void (**)(const DB *, const char *));
static void
__wt_db_get_errcall(
	DB *handle,
	void (**errcallp)(const DB *, const char *))
{
	*errcallp = handle->errcall;
}

static int __wt_db_set_errcall(
	DB *,
	void (*)(const DB *, const char *));
static int
__wt_db_set_errcall(
	DB *handle,
	void (*errcall)(const DB *, const char *))
{
	handle->errcall = errcall;
	return (0);
}

static void __wt_db_get_errfile(
	DB *,
	FILE **);
static void
__wt_db_get_errfile(
	DB *handle,
	FILE **errfilep)
{
	*errfilep = handle->errfile;
}

static int __wt_db_set_errfile(
	DB *,
	FILE *);
static int
__wt_db_set_errfile(
	DB *handle,
	FILE *errfile)
{
	handle->errfile = errfile;
	return (0);
}

static void __wt_db_get_errpfx(
	DB *,
	const char **);
static void
__wt_db_get_errpfx(
	DB *handle,
	const char **errpfxp)
{
	*errpfxp = handle->errpfx;
}

static int __wt_db_set_errpfx(
	DB *,
	const char *);
static int
__wt_db_set_errpfx(
	DB *handle,
	const char *errpfx)
{
	handle->errpfx = errpfx;
	return (0);
}

static void __wt_db_get_btree_compare(
	DB *,
	int (**)(DB *, const DBT *, const DBT *));
static void
__wt_db_get_btree_compare(
	DB *handle,
	int (**btree_comparep)(DB *, const DBT *, const DBT *))
{
	*btree_comparep = handle->btree_compare;
}

static int __wt_db_set_btree_compare(
	DB *,
	int (*)(DB *, const DBT *, const DBT *));
static int
__wt_db_set_btree_compare(
	DB *handle,
	int (*btree_compare)(DB *, const DBT *, const DBT *))
{
	handle->btree_compare = btree_compare;
	return (0);
}

static void __wt_db_get_btree_compare_int(
	DB *,
	int *);
static void
__wt_db_get_btree_compare_int(
	DB *handle,
	int *btree_compare_intp)
{
	*btree_compare_intp = handle->btree_compare_int;
}

static int __wt_db_set_btree_compare_int(
	DB *,
	int );
static int
__wt_db_set_btree_compare_int(
	DB *handle,
	int btree_compare_int)
{
	int ret;

	if ((ret = __wt_db_set_btree_compare_int_verify(
	    handle, &btree_compare_int)) != 0)
		return (ret);

	handle->btree_compare_int = btree_compare_int;
	return (0);
}

static void __wt_db_get_btree_dup_compare(
	DB *,
	int (**)(DB *, const DBT *, const DBT *));
static void
__wt_db_get_btree_dup_compare(
	DB *handle,
	int (**btree_dup_comparep)(DB *, const DBT *, const DBT *))
{
	*btree_dup_comparep = handle->btree_dup_compare;
}

static int __wt_db_set_btree_dup_compare(
	DB *,
	int (*)(DB *, const DBT *, const DBT *));
static int
__wt_db_set_btree_dup_compare(
	DB *handle,
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *))
{
	handle->btree_dup_compare = btree_dup_compare;
	return (0);
}

static void __wt_db_get_btree_itemsize(
	DB *,
	u_int32_t *,
	u_int32_t *);
static void
__wt_db_get_btree_itemsize(
	DB *handle,
	u_int32_t *intlitemsizep,
	u_int32_t *leafitemsizep)
{
	*intlitemsizep = handle->intlitemsize;
	*leafitemsizep = handle->leafitemsize;
}

static int __wt_db_set_btree_itemsize(
	DB *,
	u_int32_t ,
	u_int32_t );
static int
__wt_db_set_btree_itemsize(
	DB *handle,
	u_int32_t intlitemsize,
	u_int32_t leafitemsize)
{
	handle->intlitemsize = intlitemsize;
	handle->leafitemsize = leafitemsize;
	return (0);
}

static void __wt_db_get_btree_pagesize(
	DB *,
	u_int32_t *,
	u_int32_t *,
	u_int32_t *,
	u_int32_t *);
static void
__wt_db_get_btree_pagesize(
	DB *handle,
	u_int32_t *allocsizep,
	u_int32_t *intlsizep,
	u_int32_t *leafsizep,
	u_int32_t *extsizep)
{
	*allocsizep = handle->allocsize;
	*intlsizep = handle->intlsize;
	*leafsizep = handle->leafsize;
	*extsizep = handle->extsize;
}

static int __wt_db_set_btree_pagesize(
	DB *,
	u_int32_t ,
	u_int32_t ,
	u_int32_t ,
	u_int32_t );
static int
__wt_db_set_btree_pagesize(
	DB *handle,
	u_int32_t allocsize,
	u_int32_t intlsize,
	u_int32_t leafsize,
	u_int32_t extsize)
{
	handle->allocsize = allocsize;
	handle->intlsize = intlsize;
	handle->leafsize = leafsize;
	handle->extsize = extsize;
	return (0);
}

static void __wt_db_get_btree_dup_offpage(
	DB *,
	u_int32_t *);
static void
__wt_db_get_btree_dup_offpage(
	DB *handle,
	u_int32_t *btree_dup_offpagep)
{
	*btree_dup_offpagep = handle->btree_dup_offpage;
}

static int __wt_db_set_btree_dup_offpage(
	DB *,
	u_int32_t );
static int
__wt_db_set_btree_dup_offpage(
	DB *handle,
	u_int32_t btree_dup_offpage)
{
	handle->btree_dup_offpage = btree_dup_offpage;
	return (0);
}

void
__wt_env_config_methods(ENV *env)
{
	env->get_errcall = __wt_env_get_errcall;
	env->set_errcall = __wt_env_set_errcall;
	env->get_errfile = __wt_env_get_errfile;
	env->set_errfile = __wt_env_set_errfile;
	env->get_errpfx = __wt_env_get_errpfx;
	env->set_errpfx = __wt_env_set_errpfx;
	env->get_verbose = __wt_env_get_verbose;
	env->set_verbose = __wt_env_set_verbose;
	env->get_cachesize = __wt_env_get_cachesize;
	env->set_cachesize = __wt_env_set_cachesize;
	env->close = __wt_env_close;
	env->destroy = __wt_env_destroy;
	env->err = __wt_env_err;
	env->errx = __wt_env_errx;
	env->open = __wt_env_open;
	env->stat_clear = __wt_env_stat_clear;
	env->stat_print = __wt_env_stat_print;
}

void
__wt_env_config_methods_lockout(ENV *env)
{
	env->get_errcall = (void (*)
	    (ENV *, void (**)(const ENV *, const char *)))
	    __wt_env_lockout_err;
	env->set_errcall = (int (*)
	    (ENV *, void (*)(const ENV *, const char *)))
	    __wt_env_lockout_err;
	env->get_errfile = (void (*)
	    (ENV *, FILE **))
	    __wt_env_lockout_err;
	env->set_errfile = (int (*)
	    (ENV *, FILE *))
	    __wt_env_lockout_err;
	env->get_errpfx = (void (*)
	    (ENV *, const char **))
	    __wt_env_lockout_err;
	env->set_errpfx = (int (*)
	    (ENV *, const char *))
	    __wt_env_lockout_err;
	env->get_verbose = (void (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout_err;
	env->set_verbose = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout_err;
	env->get_cachesize = (void (*)
	    (ENV *, u_int32_t *))
	    __wt_env_lockout_err;
	env->set_cachesize = (int (*)
	    (ENV *, u_int32_t ))
	    __wt_env_lockout_err;
	env->close = (int (*)
	    (ENV *, u_int32_t))
	    __wt_env_lockout_err;
	env->err = (void (*)
	    (ENV *, int, const char *, ...))
	    __wt_env_lockout_err;
	env->errx = (void (*)
	    (ENV *, const char *, ...))
	    __wt_env_lockout_err;
	env->open = (int (*)
	    (ENV *, const char *, mode_t, u_int32_t))
	    __wt_env_lockout_err;
	env->stat_clear = (int (*)
	    (ENV *, u_int32_t))
	    __wt_env_lockout_err;
	env->stat_print = (int (*)
	    (ENV *, FILE *, u_int32_t))
	    __wt_env_lockout_err;
}

void
__wt_env_config_methods_open(ENV *env)
{
}

void
__wt_db_config_methods(DB *db)
{
	db->get_errcall = __wt_db_get_errcall;
	db->set_errcall = __wt_db_set_errcall;
	db->get_errfile = __wt_db_get_errfile;
	db->set_errfile = __wt_db_set_errfile;
	db->get_errpfx = __wt_db_get_errpfx;
	db->set_errpfx = __wt_db_set_errpfx;
	db->get_btree_compare = __wt_db_get_btree_compare;
	db->set_btree_compare = __wt_db_set_btree_compare;
	db->get_btree_compare_int = __wt_db_get_btree_compare_int;
	db->set_btree_compare_int = __wt_db_set_btree_compare_int;
	db->get_btree_dup_compare = __wt_db_get_btree_dup_compare;
	db->set_btree_dup_compare = __wt_db_set_btree_dup_compare;
	db->get_btree_itemsize = __wt_db_get_btree_itemsize;
	db->set_btree_itemsize = __wt_db_set_btree_itemsize;
	db->get_btree_pagesize = __wt_db_get_btree_pagesize;
	db->set_btree_pagesize = __wt_db_set_btree_pagesize;
	db->get_btree_dup_offpage = __wt_db_get_btree_dup_offpage;
	db->set_btree_dup_offpage = __wt_db_set_btree_dup_offpage;
	db->bulk_load = __wt_db_bulk_load;
	db->close = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_open;
	db->destroy = __wt_db_destroy;
	db->dump = (int (*)
	    (DB *, FILE *, u_int32_t))
	    __wt_db_lockout_open;
	db->err = __wt_db_err;
	db->errx = __wt_db_errx;
	db->get = (int (*)
	    (DB *, DBT *, DBT *, DBT *, u_int32_t))
	    __wt_db_lockout_open;
	db->open = __wt_db_open;
	db->stat_clear = __wt_db_stat_clear;
	db->stat_print = __wt_db_stat_print;
	db->sync = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_open;
	db->verify = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_open;
}

void
__wt_db_config_methods_lockout(DB *db)
{
	db->get_errcall = (void (*)
	    (DB *, void (**)(const DB *, const char *)))
	    __wt_db_lockout_err;
	db->set_errcall = (int (*)
	    (DB *, void (*)(const DB *, const char *)))
	    __wt_db_lockout_err;
	db->get_errfile = (void (*)
	    (DB *, FILE **))
	    __wt_db_lockout_err;
	db->set_errfile = (int (*)
	    (DB *, FILE *))
	    __wt_db_lockout_err;
	db->get_errpfx = (void (*)
	    (DB *, const char **))
	    __wt_db_lockout_err;
	db->set_errpfx = (int (*)
	    (DB *, const char *))
	    __wt_db_lockout_err;
	db->get_btree_compare = (void (*)
	    (DB *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->set_btree_compare = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->get_btree_compare_int = (void (*)
	    (DB *, int *))
	    __wt_db_lockout_err;
	db->set_btree_compare_int = (int (*)
	    (DB *, int ))
	    __wt_db_lockout_err;
	db->get_btree_dup_compare = (void (*)
	    (DB *, int (**)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->set_btree_dup_compare = (int (*)
	    (DB *, int (*)(DB *, const DBT *, const DBT *)))
	    __wt_db_lockout_err;
	db->get_btree_itemsize = (void (*)
	    (DB *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout_err;
	db->set_btree_itemsize = (int (*)
	    (DB *, u_int32_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->get_btree_pagesize = (void (*)
	    (DB *, u_int32_t *, u_int32_t *, u_int32_t *, u_int32_t *))
	    __wt_db_lockout_err;
	db->set_btree_pagesize = (int (*)
	    (DB *, u_int32_t , u_int32_t , u_int32_t , u_int32_t ))
	    __wt_db_lockout_err;
	db->get_btree_dup_offpage = (void (*)
	    (DB *, u_int32_t *))
	    __wt_db_lockout_err;
	db->set_btree_dup_offpage = (int (*)
	    (DB *, u_int32_t ))
	    __wt_db_lockout_err;
	db->bulk_load = (int (*)
	    (DB *, u_int32_t, int (*)(DB *, DBT **, DBT **)))
	    __wt_db_lockout_err;
	db->close = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_err;
	db->dump = (int (*)
	    (DB *, FILE *, u_int32_t))
	    __wt_db_lockout_err;
	db->err = (void (*)
	    (DB *, int, const char *, ...))
	    __wt_db_lockout_err;
	db->errx = (void (*)
	    (DB *, const char *, ...))
	    __wt_db_lockout_err;
	db->get = (int (*)
	    (DB *, DBT *, DBT *, DBT *, u_int32_t))
	    __wt_db_lockout_err;
	db->open = (int (*)
	    (DB *, const char *, mode_t, u_int32_t))
	    __wt_db_lockout_err;
	db->stat_clear = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_err;
	db->stat_print = (int (*)
	    (DB *, FILE *, u_int32_t))
	    __wt_db_lockout_err;
	db->sync = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_err;
	db->verify = (int (*)
	    (DB *, u_int32_t))
	    __wt_db_lockout_err;
}

void
__wt_db_config_methods_open(DB *db)
{
	db->close = __wt_db_close;
	db->dump = __wt_db_dump;
	db->get = __wt_db_get;
	db->sync = __wt_db_sync;
	db->verify = __wt_db_verify;
}
