int
__wt_db_bulk_load(WT_TOC *toc);
int
__wt_bt_close(DB *db);
int
__wt_bt_lex_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_bt_int_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_bt_dump_debug(DB *db, char *ofile, FILE *fp);
int
__wt_bt_dump_ipage(DB *db, WT_PAGE *page, char *ofile, FILE *fp);
int
__wt_bt_dump_page(DB *db, WT_PAGE *page, char *ofile, FILE *fp);
void
__wt_bt_desc_init(DB *db, WT_PAGE *page);
void
__wt_bt_desc_stats(DB *db, WT_PAGE *page);
int
__wt_bt_desc_verify(DB *db, WT_PAGE *page);
void
__wt_bt_desc_dump(WT_PAGE *page, FILE *fp);
int
__wt_bt_desc_read(DB *db);
int
__wt_bt_desc_write(DB *db, u_int32_t root_addr);
int
__wt_db_dump(WT_TOC *toc);
void
__wt_bt_print(u_int8_t *data, u_int32_t len, FILE *stream);
int
__wt_db_get(WT_TOC *toc);
int
__wt_db_get_recno(WT_TOC *toc);
int
__wt_bt_build_verify(void);
int
__wt_bt_data_copy_to_dbt(DB *db, u_int8_t *data, size_t len, DBT *copy);
void
__wt_bt_first_offp(WT_PAGE *page, u_int32_t *addrp, int *isleafp);
void
__wt_set_ff_and_sa_from_addr(DB *db, WT_PAGE *page, u_int8_t *addr);
const char *
__wt_bt_hdr_type(WT_PAGE_HDR *hdr);
const char *
__wt_bt_item_type(WT_ITEM *item);
int
__wt_bt_open(DB *db);
int
__wt_bt_ovfl_in(DB *db, u_int32_t addr, u_int32_t len, WT_PAGE **pagep);
int
__wt_bt_ovfl_write(DB *db, DBT *dbt, u_int32_t *addrp);
int
__wt_bt_ovfl_copy(DB *db, WT_ITEM_OVFL *from, WT_ITEM_OVFL *copy);
int
__wt_bt_ovfl_to_dbt(DB *db, WT_ITEM_OVFL *ovfl, DBT *copy);
int
__wt_bt_ovfl_to_indx(DB *db, WT_PAGE *page, WT_INDX *ip);
int
__wt_bt_page_alloc(DB *db, int isleaf, WT_PAGE **pagep);
int
__wt_bt_page_in(DB *db,
    WT_STOC *stoc, u_int32_t addr, int isleaf, int inmem, WT_PAGE **pagep);
int
__wt_bt_page_out(DB *db, WT_STOC *stoc, WT_PAGE *page, u_int32_t flags);
void
__wt_bt_page_recycle(ENV *env, WT_PAGE *page);
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page);
int
__wt_bt_page_inmem_append(DB *db,
    WT_PAGE *page, WT_ITEM *key_item, WT_ITEM *data_item);
int
__wt_bt_dbt_return(DB *db,
    DBT *key, DBT *data, WT_PAGE *page, WT_INDX *ip, int key_return);
int
__wt_bt_stat(DB *db);
int
__wt_db_verify(WT_TOC *toc);
int
__wt_bt_verify_int(DB *db, FILE *fp);
int
__wt_bt_verify_page(DB *db, WT_PAGE *page, bitstr_t *fragbits, FILE *fp);
int
__wt_env_start(ENV *env, u_int32_t flags);
int
__wt_env_stop(ENV *env, u_int32_t flags);
int
__wt_stoc_init(ENV *env, WT_STOC *stoc);
int
__wt_stoc_close(ENV *env, WT_STOC *stoc);
void *
__wt_workq(void *arg);
int
__wt_env_toc_create(ENV *env, u_int32_t flags, WT_TOC **tocp);
int
__wt_env_toc_sched(WT_TOC *toc);
int
__wt_db_close(WT_TOC *toc);
void
__wt_db_err(DB *db, int error, const char *fmt, ...);
void
__wt_db_errx(DB *db, const char *fmt, ...);
int
__wt_db_set_btree_compare_int_verify(WT_TOC *toc);
int
__wt_env_db_create(WT_TOC *toc);
int
__wt_db_destroy(WT_TOC *toc);
int
__wt_idb_destroy(DB *db, int refresh);
int
__wt_db_lockout_err(DB *db);
int
__wt_db_lockout_open(DB *db);
int
__wt_db_open(WT_TOC *toc);
int
__wt_db_stat_print(WT_TOC *toc);
int
__wt_db_stat_clear(WT_TOC *toc);
int
__wt_db_sync(WT_TOC *toc);
int
__wt_cache_db_open(DB *db);
int
__wt_cache_db_close(DB *db, WT_STOC *stoc);
int
__wt_cache_db_sync(DB *db, WT_STOC *stoc);
int
__wt_cache_db_alloc(DB *db, WT_STOC *stoc, u_int32_t bytes, WT_PAGE **pagep);
int
__wt_cache_db_in(DB *db, WT_STOC *stoc,
    off_t offset, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep);
int
__wt_cache_db_out(DB *db, WT_STOC *stoc, WT_PAGE *page, u_int32_t flags);
int
__wt_cache_discard(ENV *env, WT_STOC *stoc, WT_PAGE *page);
int
__wt_env_close(WT_TOC *toc);
void
__wt_env_err(ENV *env, int error, const char *fmt, ...);
void
__wt_env_errx(ENV *env, const char *fmt, ...);
int
__wt_env_set_verbose_verify(WT_TOC *toc);
int
__wt_env_set_cachesize_verify(WT_TOC *toc);
int
__wt_build_verify(void);
int
__wt_global_init(void);
int
__wt_breakpoint(void);
int
__wt_env_destroy(ENV *env, u_int32_t flags);
int
__wt_ienv_destroy(ENV *env, int refresh);
int
__wt_env_lockout_err(ENV *env);
int
__wt_env_open(WT_TOC *toc);
int
__wt_env_stat_print(WT_TOC *toc);
int
__wt_env_stat_clear(WT_TOC *toc);
void
__wt_abort(ENV *env);
int
__wt_calloc(ENV *env, u_int32_t number, u_int32_t size, void *retp);
int
__wt_realloc(ENV *env,
    u_int32_t bytes_allocated, u_int32_t bytes_to_allocate, void *retp);
int
__wt_strdup(ENV *env, const char *str, void *retp);
void
__wt_free(ENV *env, void *p);
int
__wt_filesize(ENV *env, WT_FH *fh, off_t *sizep);
int
__wt_mtx_init(WT_MTX *mtx);
int
__wt_lock(WT_MTX *mtx);
int
__wt_unlock(WT_MTX *mtx);
int
__wt_mtx_destroy(WT_MTX *mtx);
int
__wt_open(ENV *env,
    const char *name, mode_t mode, u_int32_t flags, WT_FH **fhp);
int
__wt_close(ENV *env, WT_FH *fh);
int
__wt_read(ENV *env, WT_FH *fh, off_t offset, u_int32_t bytes, void *buf);
int
__wt_write(ENV *env, WT_FH *fh, off_t offset, u_int32_t bytes, void *buf);
void
__wt_sleep(long seconds, long micro_seconds);
void
__wt_yield(void);
void
__wt_env_config_methods(ENV *env);
void
__wt_env_config_methods_open(ENV *env);
void
__wt_env_config_methods_lockout(ENV *env);
void
__wt_db_config_methods(DB *db);
void
__wt_db_config_methods_open(DB *db);
void
__wt_db_config_methods_lockout(DB *db);
void
__wt_api_switch(WT_TOC *toc);
u_int32_t
__wt_cksum(void *chunk, u_int32_t bytes);
void
__wt_errcall(void *cb, void *handle,
    const char *pfx1, const char *pfx2,
    int error, const char *fmt, va_list ap);
void
__wt_errfile(FILE *fp,
    const char *pfx1, const char *pfx2, int error, const char *fmt, va_list ap);
void
__wt_assert(ENV *env, const char *check, const char *file_name, int line_number);
int
__wt_api_flags(ENV *env, const char *name);
int
__wt_database_format(DB *db);
u_int32_t
__wt_prime(u_int32_t n);
int
__wt_stat_alloc_db_dstats(ENV *env, WT_STATS **statsp);
int
__wt_stat_clear_db_dstats(WT_STATS *stats);
int
__wt_stat_alloc_db_hstats(ENV *env, WT_STATS **statsp);
int
__wt_stat_clear_db_hstats(WT_STATS *stats);
int
__wt_stat_alloc_env_hstats(ENV *env, WT_STATS **statsp);
int
__wt_stat_clear_env_hstats(WT_STATS *stats);
int
__wt_stat_alloc_fh_stats(ENV *env, WT_STATS **statsp);
int
__wt_stat_clear_fh_stats(WT_STATS *stats);
