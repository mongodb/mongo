/* DO NOT EDIT: automatically built by dist/api.py. */

#define	wt_args_env_toc_sched(oparg)\
	toc->op = (oparg);\
	toc->env = env;\
	toc->db = NULL;\
	toc->argp = &args;\
	return (__wt_env_toc_sched(toc))
#define	wt_args_db_toc_sched(oparg)\
	toc->op = (oparg);\
	toc->env = db->env;\
	toc->db = db;\
	toc->argp = &args;\
	return (__wt_env_toc_sched(toc))

#define	WT_OP_DB_BULK_LOAD	1
typedef struct {
	u_int32_t flags;
	int (*cb)(DB *, DBT **, DBT **);
} wt_args_db_bulk_load;
#define	wt_args_db_bulk_load_pack\
	args.flags = flags;\
	args.cb = cb
#define	wt_args_db_bulk_load_unpack\
	DB *db = toc->db;\
	u_int32_t flags = ((wt_args_db_bulk_load *)(toc->argp))->flags;\
	int (*cb)(DB *, DBT **, DBT **) = ((wt_args_db_bulk_load *)(toc->argp))->cb

#define	WT_OP_DB_CLOSE	2
typedef struct {
	u_int32_t flags;
} wt_args_db_close;
#define	wt_args_db_close_pack\
	args.flags = flags
#define	wt_args_db_close_unpack\
	DB *db = toc->db;\
	u_int32_t flags = ((wt_args_db_close *)(toc->argp))->flags

#define	WT_OP_DB_DESTROY	3
typedef struct {
	u_int32_t flags;
} wt_args_db_destroy;
#define	wt_args_db_destroy_pack\
	args.flags = flags
#define	wt_args_db_destroy_unpack\
	DB *db = toc->db;\
	u_int32_t flags = ((wt_args_db_destroy *)(toc->argp))->flags

#define	WT_OP_DB_DUMP	4
typedef struct {
	FILE *stream;
	u_int32_t flags;
} wt_args_db_dump;
#define	wt_args_db_dump_pack\
	args.stream = stream;\
	args.flags = flags
#define	wt_args_db_dump_unpack\
	DB *db = toc->db;\
	FILE *stream = ((wt_args_db_dump *)(toc->argp))->stream;\
	u_int32_t flags = ((wt_args_db_dump *)(toc->argp))->flags

#define	WT_OP_DB_GET	5
typedef struct {
	DBT *key;
	DBT *pkey;
	DBT *data;
	u_int32_t flags;
} wt_args_db_get;
#define	wt_args_db_get_pack\
	args.key = key;\
	args.pkey = pkey;\
	args.data = data;\
	args.flags = flags
#define	wt_args_db_get_unpack\
	DB *db = toc->db;\
	DBT *key = ((wt_args_db_get *)(toc->argp))->key;\
	DBT *pkey = ((wt_args_db_get *)(toc->argp))->pkey;\
	DBT *data = ((wt_args_db_get *)(toc->argp))->data;\
	u_int32_t flags = ((wt_args_db_get *)(toc->argp))->flags

#define	WT_OP_DB_GET_BTREE_COMPARE	6
typedef struct {
	int (**btree_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_get_btree_compare;
#define	wt_args_db_get_btree_compare_pack\
	args.btree_compare = btree_compare
#define	wt_args_db_get_btree_compare_unpack\
	DB *db = toc->db;\
	int (**btree_compare)(DB *, const DBT *, const DBT *) = ((wt_args_db_get_btree_compare *)(toc->argp))->btree_compare

#define	WT_OP_DB_GET_BTREE_COMPARE_INT	7
typedef struct {
	int *btree_compare_int;
} wt_args_db_get_btree_compare_int;
#define	wt_args_db_get_btree_compare_int_pack\
	args.btree_compare_int = btree_compare_int
#define	wt_args_db_get_btree_compare_int_unpack\
	DB *db = toc->db;\
	int *btree_compare_int = ((wt_args_db_get_btree_compare_int *)(toc->argp))->btree_compare_int

#define	WT_OP_DB_GET_BTREE_DUP_COMPARE	8
typedef struct {
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_get_btree_dup_compare;
#define	wt_args_db_get_btree_dup_compare_pack\
	args.btree_dup_compare = btree_dup_compare
#define	wt_args_db_get_btree_dup_compare_unpack\
	DB *db = toc->db;\
	int (**btree_dup_compare)(DB *, const DBT *, const DBT *) = ((wt_args_db_get_btree_dup_compare *)(toc->argp))->btree_dup_compare

#define	WT_OP_DB_GET_BTREE_DUP_OFFPAGE	9
typedef struct {
	u_int32_t *btree_dup_offpage;
} wt_args_db_get_btree_dup_offpage;
#define	wt_args_db_get_btree_dup_offpage_pack\
	args.btree_dup_offpage = btree_dup_offpage
#define	wt_args_db_get_btree_dup_offpage_unpack\
	DB *db = toc->db;\
	u_int32_t *btree_dup_offpage = ((wt_args_db_get_btree_dup_offpage *)(toc->argp))->btree_dup_offpage

#define	WT_OP_DB_GET_BTREE_ITEMSIZE	10
typedef struct {
	u_int32_t *intlitemsize;
	u_int32_t *leafitemsize;
} wt_args_db_get_btree_itemsize;
#define	wt_args_db_get_btree_itemsize_pack\
	args.intlitemsize = intlitemsize;\
	args.leafitemsize = leafitemsize
#define	wt_args_db_get_btree_itemsize_unpack\
	DB *db = toc->db;\
	u_int32_t *intlitemsize = ((wt_args_db_get_btree_itemsize *)(toc->argp))->intlitemsize;\
	u_int32_t *leafitemsize = ((wt_args_db_get_btree_itemsize *)(toc->argp))->leafitemsize

#define	WT_OP_DB_GET_BTREE_PAGESIZE	11
typedef struct {
	u_int32_t *allocsize;
	u_int32_t *intlsize;
	u_int32_t *leafsize;
	u_int32_t *extsize;
} wt_args_db_get_btree_pagesize;
#define	wt_args_db_get_btree_pagesize_pack\
	args.allocsize = allocsize;\
	args.intlsize = intlsize;\
	args.leafsize = leafsize;\
	args.extsize = extsize
#define	wt_args_db_get_btree_pagesize_unpack\
	DB *db = toc->db;\
	u_int32_t *allocsize = ((wt_args_db_get_btree_pagesize *)(toc->argp))->allocsize;\
	u_int32_t *intlsize = ((wt_args_db_get_btree_pagesize *)(toc->argp))->intlsize;\
	u_int32_t *leafsize = ((wt_args_db_get_btree_pagesize *)(toc->argp))->leafsize;\
	u_int32_t *extsize = ((wt_args_db_get_btree_pagesize *)(toc->argp))->extsize

#define	WT_OP_DB_GET_ERRCALL	12
typedef struct {
	void (**errcall)(const DB *, const char *);
} wt_args_db_get_errcall;
#define	wt_args_db_get_errcall_pack\
	args.errcall = errcall
#define	wt_args_db_get_errcall_unpack\
	DB *db = toc->db;\
	void (**errcall)(const DB *, const char *) = ((wt_args_db_get_errcall *)(toc->argp))->errcall

#define	WT_OP_DB_GET_ERRFILE	13
typedef struct {
	FILE **errfile;
} wt_args_db_get_errfile;
#define	wt_args_db_get_errfile_pack\
	args.errfile = errfile
#define	wt_args_db_get_errfile_unpack\
	DB *db = toc->db;\
	FILE **errfile = ((wt_args_db_get_errfile *)(toc->argp))->errfile

#define	WT_OP_DB_GET_ERRPFX	14
typedef struct {
	const char **errpfx;
} wt_args_db_get_errpfx;
#define	wt_args_db_get_errpfx_pack\
	args.errpfx = errpfx
#define	wt_args_db_get_errpfx_unpack\
	DB *db = toc->db;\
	const char **errpfx = ((wt_args_db_get_errpfx *)(toc->argp))->errpfx

#define	WT_OP_DB_GET_RECNO	15
typedef struct {
	u_int64_t recno;
	DBT *key;
	DBT *pkey;
	DBT *data;
	u_int32_t flags;
} wt_args_db_get_recno;
#define	wt_args_db_get_recno_pack\
	args.recno = recno;\
	args.key = key;\
	args.pkey = pkey;\
	args.data = data;\
	args.flags = flags
#define	wt_args_db_get_recno_unpack\
	DB *db = toc->db;\
	u_int64_t recno = ((wt_args_db_get_recno *)(toc->argp))->recno;\
	DBT *key = ((wt_args_db_get_recno *)(toc->argp))->key;\
	DBT *pkey = ((wt_args_db_get_recno *)(toc->argp))->pkey;\
	DBT *data = ((wt_args_db_get_recno *)(toc->argp))->data;\
	u_int32_t flags = ((wt_args_db_get_recno *)(toc->argp))->flags

#define	WT_OP_DB_OPEN	16
typedef struct {
	const char *dbname;
	mode_t mode;
	u_int32_t flags;
} wt_args_db_open;
#define	wt_args_db_open_pack\
	args.dbname = dbname;\
	args.mode = mode;\
	args.flags = flags
#define	wt_args_db_open_unpack\
	DB *db = toc->db;\
	const char *dbname = ((wt_args_db_open *)(toc->argp))->dbname;\
	mode_t mode = ((wt_args_db_open *)(toc->argp))->mode;\
	u_int32_t flags = ((wt_args_db_open *)(toc->argp))->flags

#define	WT_OP_DB_SET_BTREE_COMPARE	17
typedef struct {
	int (*btree_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_set_btree_compare;
#define	wt_args_db_set_btree_compare_pack\
	args.btree_compare = btree_compare
#define	wt_args_db_set_btree_compare_unpack\
	DB *db = toc->db;\
	int (*btree_compare)(DB *, const DBT *, const DBT *) = ((wt_args_db_set_btree_compare *)(toc->argp))->btree_compare

#define	WT_OP_DB_SET_BTREE_COMPARE_INT	18
typedef struct {
	int btree_compare_int;
} wt_args_db_set_btree_compare_int;
#define	wt_args_db_set_btree_compare_int_pack\
	args.btree_compare_int = btree_compare_int
#define	wt_args_db_set_btree_compare_int_unpack\
	DB *db = toc->db;\
	int btree_compare_int = ((wt_args_db_set_btree_compare_int *)(toc->argp))->btree_compare_int

#define	WT_OP_DB_SET_BTREE_DUP_COMPARE	19
typedef struct {
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *);
} wt_args_db_set_btree_dup_compare;
#define	wt_args_db_set_btree_dup_compare_pack\
	args.btree_dup_compare = btree_dup_compare
#define	wt_args_db_set_btree_dup_compare_unpack\
	DB *db = toc->db;\
	int (*btree_dup_compare)(DB *, const DBT *, const DBT *) = ((wt_args_db_set_btree_dup_compare *)(toc->argp))->btree_dup_compare

#define	WT_OP_DB_SET_BTREE_DUP_OFFPAGE	20
typedef struct {
	u_int32_t btree_dup_offpage;
} wt_args_db_set_btree_dup_offpage;
#define	wt_args_db_set_btree_dup_offpage_pack\
	args.btree_dup_offpage = btree_dup_offpage
#define	wt_args_db_set_btree_dup_offpage_unpack\
	DB *db = toc->db;\
	u_int32_t btree_dup_offpage = ((wt_args_db_set_btree_dup_offpage *)(toc->argp))->btree_dup_offpage

#define	WT_OP_DB_SET_BTREE_ITEMSIZE	21
typedef struct {
	u_int32_t intlitemsize;
	u_int32_t leafitemsize;
} wt_args_db_set_btree_itemsize;
#define	wt_args_db_set_btree_itemsize_pack\
	args.intlitemsize = intlitemsize;\
	args.leafitemsize = leafitemsize
#define	wt_args_db_set_btree_itemsize_unpack\
	DB *db = toc->db;\
	u_int32_t intlitemsize = ((wt_args_db_set_btree_itemsize *)(toc->argp))->intlitemsize;\
	u_int32_t leafitemsize = ((wt_args_db_set_btree_itemsize *)(toc->argp))->leafitemsize

#define	WT_OP_DB_SET_BTREE_PAGESIZE	22
typedef struct {
	u_int32_t allocsize;
	u_int32_t intlsize;
	u_int32_t leafsize;
	u_int32_t extsize;
} wt_args_db_set_btree_pagesize;
#define	wt_args_db_set_btree_pagesize_pack\
	args.allocsize = allocsize;\
	args.intlsize = intlsize;\
	args.leafsize = leafsize;\
	args.extsize = extsize
#define	wt_args_db_set_btree_pagesize_unpack\
	DB *db = toc->db;\
	u_int32_t allocsize = ((wt_args_db_set_btree_pagesize *)(toc->argp))->allocsize;\
	u_int32_t intlsize = ((wt_args_db_set_btree_pagesize *)(toc->argp))->intlsize;\
	u_int32_t leafsize = ((wt_args_db_set_btree_pagesize *)(toc->argp))->leafsize;\
	u_int32_t extsize = ((wt_args_db_set_btree_pagesize *)(toc->argp))->extsize

#define	WT_OP_DB_SET_ERRCALL	23
typedef struct {
	void (*errcall)(const DB *, const char *);
} wt_args_db_set_errcall;
#define	wt_args_db_set_errcall_pack\
	args.errcall = errcall
#define	wt_args_db_set_errcall_unpack\
	DB *db = toc->db;\
	void (*errcall)(const DB *, const char *) = ((wt_args_db_set_errcall *)(toc->argp))->errcall

#define	WT_OP_DB_SET_ERRFILE	24
typedef struct {
	FILE *errfile;
} wt_args_db_set_errfile;
#define	wt_args_db_set_errfile_pack\
	args.errfile = errfile
#define	wt_args_db_set_errfile_unpack\
	DB *db = toc->db;\
	FILE *errfile = ((wt_args_db_set_errfile *)(toc->argp))->errfile

#define	WT_OP_DB_SET_ERRPFX	25
typedef struct {
	const char *errpfx;
} wt_args_db_set_errpfx;
#define	wt_args_db_set_errpfx_pack\
	args.errpfx = errpfx
#define	wt_args_db_set_errpfx_unpack\
	DB *db = toc->db;\
	const char *errpfx = ((wt_args_db_set_errpfx *)(toc->argp))->errpfx

#define	WT_OP_DB_STAT_CLEAR	26
typedef struct {
	u_int32_t flags;
} wt_args_db_stat_clear;
#define	wt_args_db_stat_clear_pack\
	args.flags = flags
#define	wt_args_db_stat_clear_unpack\
	DB *db = toc->db;\
	u_int32_t flags = ((wt_args_db_stat_clear *)(toc->argp))->flags

#define	WT_OP_DB_STAT_PRINT	27
typedef struct {
	FILE * stream;
	u_int32_t flags;
} wt_args_db_stat_print;
#define	wt_args_db_stat_print_pack\
	args.stream = stream;\
	args.flags = flags
#define	wt_args_db_stat_print_unpack\
	DB *db = toc->db;\
	FILE * stream = ((wt_args_db_stat_print *)(toc->argp))->stream;\
	u_int32_t flags = ((wt_args_db_stat_print *)(toc->argp))->flags

#define	WT_OP_DB_SYNC	28
typedef struct {
	u_int32_t flags;
} wt_args_db_sync;
#define	wt_args_db_sync_pack\
	args.flags = flags
#define	wt_args_db_sync_unpack\
	DB *db = toc->db;\
	u_int32_t flags = ((wt_args_db_sync *)(toc->argp))->flags

#define	WT_OP_DB_VERIFY	29
typedef struct {
	u_int32_t flags;
} wt_args_db_verify;
#define	wt_args_db_verify_pack\
	args.flags = flags
#define	wt_args_db_verify_unpack\
	DB *db = toc->db;\
	u_int32_t flags = ((wt_args_db_verify *)(toc->argp))->flags

#define	WT_OP_ENV_CLOSE	30
typedef struct {
	u_int32_t flags;
} wt_args_env_close;
#define	wt_args_env_close_pack\
	args.flags = flags
#define	wt_args_env_close_unpack\
	ENV *env = toc->env;\
	u_int32_t flags = ((wt_args_env_close *)(toc->argp))->flags

#define	WT_OP_ENV_DB_CREATE	31
typedef struct {
	u_int32_t flags;
	DB **dbp;
} wt_args_env_db_create;
#define	wt_args_env_db_create_pack\
	args.flags = flags;\
	args.dbp = dbp
#define	wt_args_env_db_create_unpack\
	ENV *env = toc->env;\
	u_int32_t flags = ((wt_args_env_db_create *)(toc->argp))->flags;\
	DB **dbp = ((wt_args_env_db_create *)(toc->argp))->dbp

#define	WT_OP_ENV_GET_CACHESIZE	32
typedef struct {
	u_int32_t *cachesize;
} wt_args_env_get_cachesize;
#define	wt_args_env_get_cachesize_pack\
	args.cachesize = cachesize
#define	wt_args_env_get_cachesize_unpack\
	ENV *env = toc->env;\
	u_int32_t *cachesize = ((wt_args_env_get_cachesize *)(toc->argp))->cachesize

#define	WT_OP_ENV_GET_ERRCALL	33
typedef struct {
	void (**errcall)(const ENV *, const char *);
} wt_args_env_get_errcall;
#define	wt_args_env_get_errcall_pack\
	args.errcall = errcall
#define	wt_args_env_get_errcall_unpack\
	ENV *env = toc->env;\
	void (**errcall)(const ENV *, const char *) = ((wt_args_env_get_errcall *)(toc->argp))->errcall

#define	WT_OP_ENV_GET_ERRFILE	34
typedef struct {
	FILE **errfile;
} wt_args_env_get_errfile;
#define	wt_args_env_get_errfile_pack\
	args.errfile = errfile
#define	wt_args_env_get_errfile_unpack\
	ENV *env = toc->env;\
	FILE **errfile = ((wt_args_env_get_errfile *)(toc->argp))->errfile

#define	WT_OP_ENV_GET_ERRPFX	35
typedef struct {
	const char **errpfx;
} wt_args_env_get_errpfx;
#define	wt_args_env_get_errpfx_pack\
	args.errpfx = errpfx
#define	wt_args_env_get_errpfx_unpack\
	ENV *env = toc->env;\
	const char **errpfx = ((wt_args_env_get_errpfx *)(toc->argp))->errpfx

#define	WT_OP_ENV_GET_VERBOSE	36
typedef struct {
	u_int32_t *verbose;
} wt_args_env_get_verbose;
#define	wt_args_env_get_verbose_pack\
	args.verbose = verbose
#define	wt_args_env_get_verbose_unpack\
	ENV *env = toc->env;\
	u_int32_t *verbose = ((wt_args_env_get_verbose *)(toc->argp))->verbose

#define	WT_OP_ENV_OPEN	37
typedef struct {
	const char *home;
	mode_t mode;
	u_int32_t flags;
} wt_args_env_open;
#define	wt_args_env_open_pack\
	args.home = home;\
	args.mode = mode;\
	args.flags = flags
#define	wt_args_env_open_unpack\
	ENV *env = toc->env;\
	const char *home = ((wt_args_env_open *)(toc->argp))->home;\
	mode_t mode = ((wt_args_env_open *)(toc->argp))->mode;\
	u_int32_t flags = ((wt_args_env_open *)(toc->argp))->flags

#define	WT_OP_ENV_SET_CACHESIZE	38
typedef struct {
	u_int32_t cachesize;
} wt_args_env_set_cachesize;
#define	wt_args_env_set_cachesize_pack\
	args.cachesize = cachesize
#define	wt_args_env_set_cachesize_unpack\
	ENV *env = toc->env;\
	u_int32_t cachesize = ((wt_args_env_set_cachesize *)(toc->argp))->cachesize

#define	WT_OP_ENV_SET_ERRCALL	39
typedef struct {
	void (*errcall)(const ENV *, const char *);
} wt_args_env_set_errcall;
#define	wt_args_env_set_errcall_pack\
	args.errcall = errcall
#define	wt_args_env_set_errcall_unpack\
	ENV *env = toc->env;\
	void (*errcall)(const ENV *, const char *) = ((wt_args_env_set_errcall *)(toc->argp))->errcall

#define	WT_OP_ENV_SET_ERRFILE	40
typedef struct {
	FILE *errfile;
} wt_args_env_set_errfile;
#define	wt_args_env_set_errfile_pack\
	args.errfile = errfile
#define	wt_args_env_set_errfile_unpack\
	ENV *env = toc->env;\
	FILE *errfile = ((wt_args_env_set_errfile *)(toc->argp))->errfile

#define	WT_OP_ENV_SET_ERRPFX	41
typedef struct {
	const char *errpfx;
} wt_args_env_set_errpfx;
#define	wt_args_env_set_errpfx_pack\
	args.errpfx = errpfx
#define	wt_args_env_set_errpfx_unpack\
	ENV *env = toc->env;\
	const char *errpfx = ((wt_args_env_set_errpfx *)(toc->argp))->errpfx

#define	WT_OP_ENV_SET_VERBOSE	42
typedef struct {
	u_int32_t verbose;
} wt_args_env_set_verbose;
#define	wt_args_env_set_verbose_pack\
	args.verbose = verbose
#define	wt_args_env_set_verbose_unpack\
	ENV *env = toc->env;\
	u_int32_t verbose = ((wt_args_env_set_verbose *)(toc->argp))->verbose

#define	WT_OP_ENV_STAT_CLEAR	43
typedef struct {
	u_int32_t flags;
} wt_args_env_stat_clear;
#define	wt_args_env_stat_clear_pack\
	args.flags = flags
#define	wt_args_env_stat_clear_unpack\
	ENV *env = toc->env;\
	u_int32_t flags = ((wt_args_env_stat_clear *)(toc->argp))->flags

#define	WT_OP_ENV_STAT_PRINT	44
typedef struct {
	FILE *stream;
	u_int32_t flags;
} wt_args_env_stat_print;
#define	wt_args_env_stat_print_pack\
	args.stream = stream;\
	args.flags = flags
#define	wt_args_env_stat_print_unpack\
	ENV *env = toc->env;\
	FILE *stream = ((wt_args_env_stat_print *)(toc->argp))->stream;\
	u_int32_t flags = ((wt_args_env_stat_print *)(toc->argp))->flags
