/* DO NOT EDIT: automatically built by dist/s_prototypes. */
int
__wt_db_bulk_load(DB *db, u_int32_t flags, int (*cb)(DB *, DBT **, DBT **));
int
__wt_bt_close(DB *db);
int
__wt_bt_lex_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_bt_int_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_diag_set_fp(const char *ofile, FILE **fpp, int *close_varp);
int
__wt_bt_dump_debug(DB *db, char *ofile, FILE *fp);
int
__wt_bt_dump_page(DB *db, WT_PAGE *page, char *ofile, FILE *fp, int inmemory);
void
__wt_bt_desc_init(DB *db, WT_PAGE *page);
void
__wt_bt_desc_stats(DB *db, WT_PAGE *page);
int
__wt_bt_desc_verify(DB *db, WT_PAGE *page);
void
__wt_bt_desc_dump(WT_PAGE *page, FILE *fp);
int
__wt_bt_desc_read(WT_TOC *toc);
int
__wt_bt_desc_write(WT_TOC *toc, u_int32_t root_addr);
int
__wt_db_dump(DB *db, FILE *stream, u_int32_t flags);
void
__wt_bt_print(u_int8_t *data, u_int32_t len, FILE *stream);
int
__wt_db_get(
    DB *db, WT_TOC *toc, DBT *key, DBT *pkey, DBT *data, u_int32_t flags);
int
__wt_db_get_recno(DB *db, WT_TOC *toc,
    u_int64_t recno, DBT *key, DBT *pkey, DBT *data, u_int32_t flags);
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
__wt_bt_open(DB *db, int ok_create);
int
__wt_bt_root_page(WT_TOC *toc);
int
__wt_bt_ovfl_in(WT_TOC *toc, u_int32_t addr, u_int32_t len, WT_PAGE **pagep);
int
__wt_bt_ovfl_write(WT_TOC *toc, DBT *dbt, u_int32_t *addrp);
int
__wt_bt_ovfl_copy(WT_TOC *toc, WT_ITEM_OVFL *from, WT_ITEM_OVFL *copy);
int
__wt_bt_ovfl_to_dbt(WT_TOC *toc, WT_ITEM_OVFL *ovfl, DBT *copy);
int
__wt_bt_ovfl_to_indx(WT_TOC *toc, WT_PAGE *page, WT_INDX *ip);
int
__wt_bt_page_alloc(WT_TOC *toc, int isleaf, WT_PAGE **pagep);
int
__wt_bt_page_in(
    WT_TOC *toc, u_int32_t addr, int isleaf, int inmem, WT_PAGE **pagep);
int
__wt_bt_page_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags);
void
__wt_bt_page_recycle(ENV *env, WT_PAGE *page);
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page);
int
__wt_bt_page_inmem_append(DB *db,
    WT_PAGE *page, WT_ITEM *key_item, WT_ITEM *data_item);
int
__wt_bt_dbt_return(WT_TOC *toc,
    DBT *key, DBT *data, WT_PAGE *page, WT_INDX *ip, int key_return);
int
__wt_bt_stat(DB *db);
int
__wt_bt_sync(DB *db, void (*f)(const char *, u_int32_t));
int
__wt_db_verify(DB *db, void (*f)(const char *s, u_int32_t), u_int32_t flags);
int
__wt_db_verify_int(DB *db,
    void (*f)(const char *s, u_int32_t), FILE *fp, u_int32_t flags);
int
__wt_bt_verify_page(WT_TOC *toc, WT_PAGE *page, void *vs_arg);
void
__wt_db_err(DB *db, int error, const char *fmt, ...);
void
__wt_db_errx(DB *db, const char *fmt, ...);
int
__wt_db_btree_compare_int_set_verify(DB *db, int btree_compare_int);
int
__wt_env_db(ENV *env, u_int32_t flags, DB **dbp);
int
__wt_db_close(DB *db, u_int32_t flags);
int
__wt_idb_close(DB *db, int refresh);
int
__wt_db_lockout_err(DB *db);
int
__wt_db_lockout_open(DB *db);
int
__wt_db_open(DB *db, const char *dbname, mode_t mode, u_int32_t flags);
int
__wt_db_stat_print(DB *db, FILE *stream, u_int32_t flags);
int
__wt_db_stat_clear(DB *db, u_int32_t flags);
int
__wt_db_sync(DB *db, void (*f)(const char *, u_int32_t), u_int32_t flags);
void *
__wt_workq_srvr(void *arg);
int
__wt_env_toc(ENV *env, u_int32_t flags, WT_TOC **tocp);
int
__wt_wt_toc_close(WT_TOC *toc, u_int32_t flags);
int
__wt_toc_dump(ENV *env, const char *ofile, FILE *fp);
int
__wt_cache_create(ENV *env);
int
__wt_cache_destroy(ENV *env);
int
__wt_cache_sync(WT_TOC *toc, WT_FH *fh, void (*f)(const char *, u_int32_t));
int
__wt_cache_alloc(WT_TOC *toc, u_int32_t bytes, WT_PAGE **pagep);
int
__wt_cache_in(WT_TOC *toc,
    off_t offset, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep);
int
__wt_cache_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags);
void *
__wt_cache_srvr(void *arg);
void
__wt_api_env_err(ENV *env, int error, const char *fmt, ...);
void
__wt_api_env_errx(ENV *env, const char *fmt, ...);
int
__wt_env_verbose_set_verify(ENV *env, u_int32_t verbose);
int
__wt_library_init(void);
int
__wt_breakpoint(void);
int
__wt_env_create(u_int32_t flags, ENV **envp);
int
__wt_ienv_destroy(ENV *env, int refresh);
void
__wt_msg(ENV *env, const char *fmt, ...);
int
__wt_env_open(ENV *env, const char *home, mode_t mode, u_int32_t flags);
int
__wt_env_close(ENV *env, u_int32_t flags);
int
__wt_env_stat_print(ENV *env, FILE *stream, u_int32_t flags);
int
__wt_env_stat_clear(ENV *env, u_int32_t flags);
void
__wt_stat_print(ENV *env, WT_STATS *s, FILE *stream);
void
__wt_abort();
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
void
__wt_lock(ENV *env, WT_MTX *mtx);
void
__wt_unlock(WT_MTX *mtx);
int
__wt_mtx_destroy(WT_MTX *mtx);
int
__wt_open(ENV *env, const char *name, mode_t mode, int ok_create, WT_FH **fhp);
int
__wt_close(ENV *env, WT_FH *fh);
int
__wt_read(ENV *env, WT_FH *fh, off_t offset, u_int32_t bytes, void *buf);
int
__wt_write(ENV *env, WT_FH *fh, off_t offset, u_int32_t bytes, void *buf);
void
__wt_sleep(long seconds, long micro_seconds);
int
__wt_thread_create(pthread_t *tidret, void *(*func)(void *), void *arg);
void
__wt_thread_join(pthread_t tid);
void
__wt_yield(void);
void
__wt_methods_db_lockout(DB *db);
void
__wt_methods_db_init_transition(DB *db);
void
__wt_methods_db_open_transition(DB *db);
void
__wt_methods_env_lockout(ENV *env);
void
__wt_methods_env_init_transition(ENV *env);
void
__wt_methods_env_open_transition(ENV *env);
void
__wt_methods_wt_toc_lockout(WT_TOC *wt_toc);
void
__wt_methods_wt_toc_init_transition(WT_TOC *wt_toc);
u_int32_t
__wt_cksum(void *chunk, u_int32_t bytes);
void
__wt_msg_call(void *cb, void *handle,
    const char *pfx1, const char *pfx2,
    int error, const char *fmt, va_list ap);
void
__wt_msg_stream(FILE *fp,
    const char *pfx1, const char *pfx2, int error, const char *fmt, va_list ap);
void
__wt_assert(ENV *env, const char *check, const char *file_name, int line_number);
int
__wt_api_flags(ENV *env, const char *name);
int
__wt_database_format(DB *db);
int
__wt_wt_toc_lockout(WT_TOC *toc);
int
__wt_db_lockout(DB *db);
int
__wt_env_lockout(ENV *env);
u_int32_t
__wt_prime(u_int32_t n);
int
__wt_stat_alloc_fh_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_fh_stats(WT_STATS *stats);
int
__wt_stat_alloc_idb_dstats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_idb_dstats(WT_STATS *stats);
int
__wt_stat_alloc_idb_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_idb_stats(WT_STATS *stats);
int
__wt_stat_alloc_ienv_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_ienv_stats(WT_STATS *stats);
