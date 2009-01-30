/* DO NOT EDIT: automatically built by dist/api.py. */

#define	WT_OP_DB_BULK_LOAD	1
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t flags;
	int (*cb)(DB *, DBT **, DBT **);
} wt_args_db_bulk_load;
#define	wt_args_db_bulk_load_unpack\
	DB *db = argp->db;\
	u_int32_t flags = argp->flags;\
	int (*cb)(DB *, DBT **, DBT **) = argp->cb

#define	WT_OP_DB_CLOSE	2
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t flags;
} wt_args_db_close;
#define	wt_args_db_close_unpack\
	DB *db = argp->db;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_DESTROY	3
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t flags;
} wt_args_db_destroy;
#define	wt_args_db_destroy_unpack\
	DB *db = argp->db;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_DUMP	4
typedef struct {
	int op;
	int ret;
	DB *db;
	FILE *stream;
	u_int32_t flags;
} wt_args_db_dump;
#define	wt_args_db_dump_unpack\
	DB *db = argp->db;\
	FILE *stream = argp->stream;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_GET	5
typedef struct {
	int op;
	int ret;
	DB *db;
	DBT *key;
	DBT *pkey;
	DBT *data;
	u_int32_t flags;
} wt_args_db_get;
#define	wt_args_db_get_unpack\
	DB *db = argp->db;\
	DBT *key = argp->key;\
	DBT *pkey = argp->pkey;\
	DBT *data = argp->data;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_GET_BTREE_COMPARE	6
typedef struct {
	int op;
	int ret;
	DB *db;
	int (**btree_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_get_btree_compare;
#define	wt_args_db_get_btree_compare_unpack\
	DB *db = argp->db;\
	int (**btree_compare)(DB *, const DBT *, const DBT *) = argp->btree_compare

#define	WT_OP_DB_GET_BTREE_COMPARE_INT	7
typedef struct {
	int op;
	int ret;
	DB *db;
	int *btree_compare_int;
} wt_args_db_get_btree_compare_int;
#define	wt_args_db_get_btree_compare_int_unpack\
	DB *db = argp->db;\
	int *btree_compare_int = argp->btree_compare_int

#define	WT_OP_DB_GET_BTREE_DUP_COMPARE	8
typedef struct {
	int op;
	int ret;
	DB *db;
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_get_btree_dup_compare;
#define	wt_args_db_get_btree_dup_compare_unpack\
	DB *db = argp->db;\
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *) = argp->btree_dup_compare

#define	WT_OP_DB_GET_BTREE_DUP_OFFPAGE	9
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t *btree_dup_offpage;
} wt_args_db_get_btree_dup_offpage;
#define	wt_args_db_get_btree_dup_offpage_unpack\
	DB *db = argp->db;\
	u_int32_t *btree_dup_offpage = argp->btree_dup_offpage

#define	WT_OP_DB_GET_BTREE_ITEMSIZE	10
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t *intlitemsize;
	u_int32_t *leafitemsize;
} wt_args_db_get_btree_itemsize;
#define	wt_args_db_get_btree_itemsize_unpack\
	DB *db = argp->db;\
	u_int32_t *intlitemsize = argp->intlitemsize;\
	u_int32_t *leafitemsize = argp->leafitemsize

#define	WT_OP_DB_GET_BTREE_PAGESIZE	11
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t *allocsize;
	u_int32_t *intlsize;
	u_int32_t *leafsize;
	u_int32_t *extsize;
} wt_args_db_get_btree_pagesize;
#define	wt_args_db_get_btree_pagesize_unpack\
	DB *db = argp->db;\
	u_int32_t *allocsize = argp->allocsize;\
	u_int32_t *intlsize = argp->intlsize;\
	u_int32_t *leafsize = argp->leafsize;\
	u_int32_t *extsize = argp->extsize

#define	WT_OP_DB_GET_ERRCALL	12
typedef struct {
	int op;
	int ret;
	DB *db;
	void (**errcall)(const DB *, const char *);
} wt_args_db_get_errcall;
#define	wt_args_db_get_errcall_unpack\
	DB *db = argp->db;\
	void (**errcall)(const DB *, const char *) = argp->errcall

#define	WT_OP_DB_GET_ERRFILE	13
typedef struct {
	int op;
	int ret;
	DB *db;
	FILE **errfile;
} wt_args_db_get_errfile;
#define	wt_args_db_get_errfile_unpack\
	DB *db = argp->db;\
	FILE **errfile = argp->errfile

#define	WT_OP_DB_GET_ERRPFX	14
typedef struct {
	int op;
	int ret;
	DB *db;
	const char **errpfx;
} wt_args_db_get_errpfx;
#define	wt_args_db_get_errpfx_unpack\
	DB *db = argp->db;\
	const char **errpfx = argp->errpfx

#define	WT_OP_DB_OPEN	15
typedef struct {
	int op;
	int ret;
	DB *db;
	const char *dbname;
	mode_t mode;
	u_int32_t flags;
} wt_args_db_open;
#define	wt_args_db_open_unpack\
	DB *db = argp->db;\
	const char *dbname = argp->dbname;\
	mode_t mode = argp->mode;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_SET_BTREE_COMPARE	16
typedef struct {
	int op;
	int ret;
	DB *db;
	int (*btree_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_set_btree_compare;
#define	wt_args_db_set_btree_compare_unpack\
	DB *db = argp->db;\
	int (*btree_compare)(DB *, const DBT *, const DBT *) = argp->btree_compare

#define	WT_OP_DB_SET_BTREE_COMPARE_INT	17
typedef struct {
	int op;
	int ret;
	DB *db;
	int btree_compare_int;
} wt_args_db_set_btree_compare_int;
#define	wt_args_db_set_btree_compare_int_unpack\
	DB *db = argp->db;\
	int btree_compare_int = argp->btree_compare_int

#define	WT_OP_DB_SET_BTREE_DUP_COMPARE	18
typedef struct {
	int op;
	int ret;
	DB *db;
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_set_btree_dup_compare;
#define	wt_args_db_set_btree_dup_compare_unpack\
	DB *db = argp->db;\
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *) = argp->btree_dup_compare

#define	WT_OP_DB_SET_BTREE_DUP_OFFPAGE	19
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t btree_dup_offpage;
} wt_args_db_set_btree_dup_offpage;
#define	wt_args_db_set_btree_dup_offpage_unpack\
	DB *db = argp->db;\
	u_int32_t btree_dup_offpage = argp->btree_dup_offpage

#define	WT_OP_DB_SET_BTREE_ITEMSIZE	20
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t intlitemsize;
	u_int32_t leafitemsize;
} wt_args_db_set_btree_itemsize;
#define	wt_args_db_set_btree_itemsize_unpack\
	DB *db = argp->db;\
	u_int32_t intlitemsize = argp->intlitemsize;\
	u_int32_t leafitemsize = argp->leafitemsize

#define	WT_OP_DB_SET_BTREE_PAGESIZE	21
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t allocsize;
	u_int32_t intlsize;
	u_int32_t leafsize;
	u_int32_t extsize;
} wt_args_db_set_btree_pagesize;
#define	wt_args_db_set_btree_pagesize_unpack\
	DB *db = argp->db;\
	u_int32_t allocsize = argp->allocsize;\
	u_int32_t intlsize = argp->intlsize;\
	u_int32_t leafsize = argp->leafsize;\
	u_int32_t extsize = argp->extsize

#define	WT_OP_DB_SET_ERRCALL	22
typedef struct {
	int op;
	int ret;
	DB *db;
	void (*errcall)(const DB *, const char *);
} wt_args_db_set_errcall;
#define	wt_args_db_set_errcall_unpack\
	DB *db = argp->db;\
	void (*errcall)(const DB *, const char *) = argp->errcall

#define	WT_OP_DB_SET_ERRFILE	23
typedef struct {
	int op;
	int ret;
	DB *db;
	FILE *errfile;
} wt_args_db_set_errfile;
#define	wt_args_db_set_errfile_unpack\
	DB *db = argp->db;\
	FILE *errfile = argp->errfile

#define	WT_OP_DB_SET_ERRPFX	24
typedef struct {
	int op;
	int ret;
	DB *db;
	const char *errpfx;
} wt_args_db_set_errpfx;
#define	wt_args_db_set_errpfx_unpack\
	DB *db = argp->db;\
	const char *errpfx = argp->errpfx

#define	WT_OP_DB_STAT_CLEAR	25
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t flags;
} wt_args_db_stat_clear;
#define	wt_args_db_stat_clear_unpack\
	DB *db = argp->db;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_STAT_PRINT	26
typedef struct {
	int op;
	int ret;
	DB *db;
	FILE * stream;
	u_int32_t flags;
} wt_args_db_stat_print;
#define	wt_args_db_stat_print_unpack\
	DB *db = argp->db;\
	FILE * stream = argp->stream;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_SYNC	27
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t flags;
} wt_args_db_sync;
#define	wt_args_db_sync_unpack\
	DB *db = argp->db;\
	u_int32_t flags = argp->flags

#define	WT_OP_DB_VERIFY	28
typedef struct {
	int op;
	int ret;
	DB *db;
	u_int32_t flags;
} wt_args_db_verify;
#define	wt_args_db_verify_unpack\
	DB *db = argp->db;\
	u_int32_t flags = argp->flags

#define	WT_OP_ENV_CLOSE	29
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t flags;
} wt_args_env_close;
#define	wt_args_env_close_unpack\
	ENV *env = argp->env;\
	u_int32_t flags = argp->flags

#define	WT_OP_ENV_DESTROY	30
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t flags;
} wt_args_env_destroy;
#define	wt_args_env_destroy_unpack\
	ENV *env = argp->env;\
	u_int32_t flags = argp->flags

#define	WT_OP_ENV_GET_CACHESIZE	31
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t *cachesize;
} wt_args_env_get_cachesize;
#define	wt_args_env_get_cachesize_unpack\
	ENV *env = argp->env;\
	u_int32_t *cachesize = argp->cachesize

#define	WT_OP_ENV_GET_ERRCALL	32
typedef struct {
	int op;
	int ret;
	ENV *env;
	void (**errcall)(const ENV *, const char *);
} wt_args_env_get_errcall;
#define	wt_args_env_get_errcall_unpack\
	ENV *env = argp->env;\
	void (**errcall)(const ENV *, const char *) = argp->errcall

#define	WT_OP_ENV_GET_ERRFILE	33
typedef struct {
	int op;
	int ret;
	ENV *env;
	FILE **errfile;
} wt_args_env_get_errfile;
#define	wt_args_env_get_errfile_unpack\
	ENV *env = argp->env;\
	FILE **errfile = argp->errfile

#define	WT_OP_ENV_GET_ERRPFX	34
typedef struct {
	int op;
	int ret;
	ENV *env;
	const char **errpfx;
} wt_args_env_get_errpfx;
#define	wt_args_env_get_errpfx_unpack\
	ENV *env = argp->env;\
	const char **errpfx = argp->errpfx

#define	WT_OP_ENV_GET_VERBOSE	35
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t *verbose;
} wt_args_env_get_verbose;
#define	wt_args_env_get_verbose_unpack\
	ENV *env = argp->env;\
	u_int32_t *verbose = argp->verbose

#define	WT_OP_ENV_OPEN	36
typedef struct {
	int op;
	int ret;
	ENV *env;
	const char *home;
	mode_t mode;
	u_int32_t flags;
} wt_args_env_open;
#define	wt_args_env_open_unpack\
	ENV *env = argp->env;\
	const char *home = argp->home;\
	mode_t mode = argp->mode;\
	u_int32_t flags = argp->flags

#define	WT_OP_ENV_SET_CACHESIZE	37
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t cachesize;
} wt_args_env_set_cachesize;
#define	wt_args_env_set_cachesize_unpack\
	ENV *env = argp->env;\
	u_int32_t cachesize = argp->cachesize

#define	WT_OP_ENV_SET_ERRCALL	38
typedef struct {
	int op;
	int ret;
	ENV *env;
	void (*errcall)(const ENV *, const char *);
} wt_args_env_set_errcall;
#define	wt_args_env_set_errcall_unpack\
	ENV *env = argp->env;\
	void (*errcall)(const ENV *, const char *) = argp->errcall

#define	WT_OP_ENV_SET_ERRFILE	39
typedef struct {
	int op;
	int ret;
	ENV *env;
	FILE *errfile;
} wt_args_env_set_errfile;
#define	wt_args_env_set_errfile_unpack\
	ENV *env = argp->env;\
	FILE *errfile = argp->errfile

#define	WT_OP_ENV_SET_ERRPFX	40
typedef struct {
	int op;
	int ret;
	ENV *env;
	const char *errpfx;
} wt_args_env_set_errpfx;
#define	wt_args_env_set_errpfx_unpack\
	ENV *env = argp->env;\
	const char *errpfx = argp->errpfx

#define	WT_OP_ENV_SET_VERBOSE	41
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t verbose;
} wt_args_env_set_verbose;
#define	wt_args_env_set_verbose_unpack\
	ENV *env = argp->env;\
	u_int32_t verbose = argp->verbose

#define	WT_OP_ENV_STAT_CLEAR	42
typedef struct {
	int op;
	int ret;
	ENV *env;
	u_int32_t flags;
} wt_args_env_stat_clear;
#define	wt_args_env_stat_clear_unpack\
	ENV *env = argp->env;\
	u_int32_t flags = argp->flags

#define	WT_OP_ENV_STAT_PRINT	43
typedef struct {
	int op;
	int ret;
	ENV *env;
	FILE *stream;
	u_int32_t flags;
} wt_args_env_stat_print;
#define	wt_args_env_stat_print_unpack\
	ENV *env = argp->env;\
	FILE *stream = argp->stream;\
	u_int32_t flags = argp->flags
