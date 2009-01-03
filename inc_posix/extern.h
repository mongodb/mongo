int
__wt_db_bulk_load(DB *db, u_int32_t flags, int (*cb)(DB *, DBT **, DBT **));
int
__wt_bt_close(DB *db);
int
__wt_lex_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_int_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_db_force_load(void);
int
__wt_db_dump_debug(DB *db, char *ofile, FILE *fp);
void
__wt_db_desc_init(DB *db, WT_PAGE *page);
int
__wt_db_desc_verify(DB *db, WT_PAGE *page);
int
__wt_db_desc_set_root(DB *db, u_int32_t root_addr);
int
__wt_db_desc_read(DB *db, WT_PAGE_DESC *desc);
int
__wt_db_dump(DB *db, FILE *stream, u_int32_t flags);
void
__wt_db_print(u_int8_t *data, u_int32_t len, FILE *stream);
int
__wt_db_page_open(IDB *idb);
int
__wt_db_page_sync(DB *db);
int
__wt_db_page_close(DB *db);
int
__wt_db_page_alloc(DB *db, u_int32_t frags, WT_PAGE **pagep);
int
__wt_db_page_in(DB *db,
    u_int32_t addr, u_int32_t frags, WT_PAGE **pagep, u_int32_t flags);
int
__wt_db_page_out(DB *db, WT_PAGE *page, u_int32_t flags);
int
__wt_db_get(DB *db, DBT *key, DBT *pkey, DBT *data, u_int32_t flags);
int
__wt_db_build_verify(void);
int
__wt_datalen_copy_to_dbt(DB *db, u_int8_t *data, size_t len, DBT *copy);
void
__wt_first_offp_addr(WT_PAGE *page, u_int32_t *addrp);
const char *
__wt_db_hdr_type(u_int32_t type);
const char *
__wt_db_item_type(u_int32_t type);
int
__wt_bt_open(DB *db);
int
__wt_db_ovfl_write(DB *db, DBT *dbt, u_int32_t *addrp);
int
__wt_db_ovfl_copy(DB *db, WT_ITEM_OVFL *from, WT_ITEM_OVFL *copy);
int
__wt_db_ovfl_item_copy(DB *db, WT_ITEM_OVFL *ovfl, DBT *copy);
int
__wt_page_inmem(DB *db, WT_PAGE *page);
void
__wt_page_inmem_alloc(DB *db, WT_PAGE *page);
int
__wt_dbt_return(DB *db, DBT *data, WT_PAGE *page, WT_INDX *indx);
int
__wt_db_verify(DB *db, u_int32_t flags);
int
__wt_db_verify_page(DB *db, WT_PAGE *page, bitstr_t *fragbits);
int
__wt_db_close(DB *db, u_int32_t flags);
void
__wt_db_err(DB *db, int error, const char *fmt, ...);
void
__wt_db_errx(DB *db, const char *fmt, ...);
int
__wt_db_set_btree_compare_int_verify(DB *db, int *bytesp);
int
__wt_db_set_pagesize_verify(DB *db, u_int32_t *pagesizep,
    u_int32_t *fragsizep, u_int32_t *extentsizep, u_int32_t *maxitemsizep);
int
__wt_db_destroy(DB *db, u_int32_t flags);
void
__wt_idb_destroy(DB *db, int refresh);
int
__wt_db_lockout_err(DB *db);
int
__wt_db_lockout_open(DB *db);
int
__wt_db_open(DB *db, const char *file_name, mode_t mode, u_int32_t flags);
int
__wt_db_stat_print(DB *db, FILE *fp, u_int32_t flags);
int
__wt_db_stat_clear(DB *db, u_int32_t flags);
int
__wt_db_sync(DB *db, u_int32_t flags);
int
__wt_env_close(ENV *env, u_int32_t flags);
void
__wt_env_err(ENV *env, int error, const char *fmt, ...);
void
__wt_env_errx(ENV *env, const char *fmt, ...);
int
__wt_env_set_verbose_verify(ENV *env, u_int32_t *whichp);
int
__wt_env_build_verify(void);
int
__wt_env_destroy(ENV *env, u_int32_t flags);
int
__wt_env_lockout_err(ENV *env);
int
__wt_env_open(ENV *env, const char *home, mode_t mode, u_int32_t flags);
void
__wt_abort(IENV *ienv);
int
__wt_calloc(IENV *ienv, size_t number, size_t size, void *retp);
int
__wt_malloc(IENV *ienv, size_t bytes_to_allocate, void *retp);
int
__wt_realloc(IENV *ienv, size_t bytes_to_allocate, void *retp);
int
__wt_strdup(IENV *ienv, const char *str, void *retp);
void
__wt_free(IENV *ienv, void *p);
int
__wt_filesize(IENV *ienv, WT_FH *fh, off_t *sizep);
int
__wt_open(IENV *ienv,
    const char *name, mode_t mode, u_int32_t flags, WT_FH **fhp);
int
__wt_close(IENV *ienv, WT_FH *fh);
int
__wt_read(IENV *ienv, WT_FH *fh, off_t offset, size_t bytes, void *buf);
int
__wt_write(IENV *ienv, WT_FH *fh, off_t offset, size_t bytes, void *buf);
u_int32_t
__wt_cksum(void *chunk, size_t len);
void
__wt_errcall(void *cb, void *handle,
    const char *pfx1, const char *pfx2,
    int error, const char *fmt, va_list ap);
void
__wt_errfile(FILE *fp,
    const char *pfx1, const char *pfx2, int error, const char *fmt, va_list ap);
void
__wt_assert(
    IENV *ienv, const char *check, const char *file_name, int line_number);
int
__wt_api_flags(IENV *ienv, const char *name);
int
__wt_database_format(DB *db);
void
__wt_env_config_methods(ENV *env);
void
__wt_env_config_methods_lockout(ENV *env);
void
__wt_env_config_methods_open(ENV *env);
void
__wt_db_config_methods(DB *db);
void
__wt_db_config_methods_lockout(DB *db);
void
__wt_db_config_methods_open(DB *db);
int
__wt_stat_alloc_fh(IENV *ienv, WT_STATS **statsp);
int
__wt_stat_clear_fh(WT_STATS *stats);
int
__wt_stat_alloc_db(IENV *ienv, WT_STATS **statsp);
int
__wt_stat_clear_db(WT_STATS *stats);
