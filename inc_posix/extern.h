/* DO NOT EDIT: automatically built by dist/s_prototypes. */
int
__wt_db_bulk_load(WT_TOC *toc, u_int32_t flags,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **));
int
__wt_bt_build_key_item(
    WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl, int bulk_load);
int
__wt_bt_build_data_item(
    WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl, u_int flags);
int
__wt_bt_close(WT_TOC *toc);
int
__wt_bt_lex_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_bt_int_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_bt_debug_dump(WT_TOC *toc, char *ofile, FILE *fp);
int
__wt_bt_debug_addr(
    WT_TOC *toc, u_int32_t addr, u_int32_t size, char *ofile, FILE *fp);
int
__wt_bt_debug_page(WT_TOC *toc, WT_PAGE *page, char *ofile, FILE *fp);
int
__wt_bt_debug_inmem(WT_TOC *toc, WT_PAGE *page, char *ofile, FILE *fp);
void
__wt_bt_debug_dbt(const char *tag, void *arg_dbt, FILE *fp);
int
__wt_bt_stat_desc(WT_TOC *toc);
int
__wt_bt_desc_read(WT_TOC *toc);
int
__wt_bt_desc_write_root(WT_TOC *toc, u_int32_t root_addr, u_int32_t root_size);
int
__wt_bt_desc_write(WT_TOC *toc);
void
__wt_bt_page_discard(ENV *env, WT_PAGE *page);
int
__wt_db_dump(WT_TOC *toc,
    FILE *stream, void (*f)(const char *, u_int64_t), u_int32_t flags);
void
__wt_bt_print(u_int8_t *data, u_int32_t size, FILE *stream);
int
__wt_bt_build_verify(void);
int
__wt_bt_data_copy_to_dbt(DB *db, u_int8_t *data, size_t len, DBT *copy);
inline void
__wt_bt_set_ff_and_sa_from_offset(WT_PAGE *page, u_int8_t *p);
inline int
__wt_page_write_gen_update(WT_PAGE *page, u_int32_t write_gen);
const char *
__wt_bt_hdr_type(WT_PAGE_HDR *hdr);
const char *
__wt_bt_item_type(WT_ITEM *item);
int
__wt_bt_open(WT_TOC *toc, int ok_create);
int
__wt_bt_root_pin(WT_TOC *toc, int pin);
int
__wt_bt_ovfl_in(WT_TOC *toc, WT_OVFL *ovfl, WT_PAGE **pagep);
int
__wt_bt_ovfl_write(WT_TOC *toc, DBT *dbt, u_int32_t *addrp);
int
__wt_bt_ovfl_copy(WT_TOC *toc, WT_OVFL *from, WT_OVFL *copy);
int
__wt_bt_page_alloc(
    WT_TOC *toc, u_int type, u_int level, u_int32_t size, WT_PAGE **pagep);
int
__wt_bt_page_in(
    WT_TOC *toc, u_int32_t addr, u_int32_t size, int inmem, WT_PAGE **pagep);
void
__wt_bt_page_out(WT_TOC *toc, WT_PAGE **pagep, u_int32_t flags);
int
__wt_bt_page_inmem(DB *db, WT_PAGE *page);
int
__wt_bt_key_process(WT_TOC *toc, WT_PAGE *page, WT_ROW *rip, DBT *dbt);
int
__wt_bt_rec_page(WT_TOC *toc, WT_PAGE *page);
int
__wt_bt_dbt_return(WT_TOC *toc, DBT *key, DBT *data, int key_return);
int
__wt_bt_tree_walk(WT_TOC *toc, u_int32_t addr,
    u_int32_t size, int (*work)(WT_TOC *, WT_PAGE *, void *), void *arg);
int
__wt_bt_stat_page(WT_TOC *toc, WT_PAGE *page, void *arg);
int
__wt_bt_sync(WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags);
int
__wt_db_verify(WT_TOC *toc, void (*f)(const char *, u_int64_t));
int
__wt_bt_verify(
    WT_TOC *toc, void (*f)(const char *, u_int64_t), FILE *stream);
int
__wt_bt_verify_page(WT_TOC *toc, WT_PAGE *page, void *vs_arg);
int
__wt_db_col_get(WT_TOC *toc, u_int64_t recno, DBT *data);
inline int
__wt_db_col_del(WT_TOC *toc, u_int64_t recno);
inline int
__wt_db_col_put(WT_TOC *toc, u_int64_t recno, DBT *data);
int
__wt_bt_rcc_expand_serial_func(WT_TOC *toc);
int
__wt_bt_rcc_expand_repl_serial_func(WT_TOC *toc);
int
__wt_bt_search_col(WT_TOC *toc, u_int64_t recno, u_int32_t flags);
int
__wt_db_row_get(WT_TOC *toc, DBT *key, DBT *data);
inline int
__wt_db_row_del(WT_TOC *toc, DBT *key);
inline int
__wt_db_row_put(WT_TOC *toc, DBT *key, DBT *data);
int
__wt_bt_update_serial_func(WT_TOC *toc);
int
__wt_bt_repl_alloc(WT_TOC *toc, WT_REPL **replp, DBT *data);
void
__wt_bt_repl_free(WT_TOC *toc, WT_REPL *repl);
int
__wt_bt_search_row(WT_TOC *toc, DBT *key, u_int32_t flags);
int
__wt_cache_alloc(WT_TOC *toc, u_int32_t *addrp, u_int32_t size);
int
__wt_cache_free(WT_TOC *toc, u_int32_t addr, u_int32_t size);
void
__wt_workq_drain_server(ENV *env, int force);
void *
__wt_cache_drain_server(void *arg);
void
__wt_drain_dump(ENV *env, const char *tag);
int
__wt_cache_create(ENV *env);
u_int64_t
__wt_cache_pages_inuse(WT_CACHE *cache);
u_int64_t
__wt_cache_bytes_inuse(WT_CACHE *cache);
void
__wt_cache_stats(ENV *env);
int
__wt_cache_destroy(ENV *env);
void
__wt_cache_dump(ENV *env);
int
__wt_page_alloc(WT_TOC *toc, u_int32_t size, WT_PAGE **pagep);
int
__wt_page_in(WT_TOC *toc,
    u_int32_t addr, u_int32_t size, WT_PAGE **pagep, u_int32_t flags);
void
__wt_page_out(WT_TOC *toc, WT_PAGE *page);
int
__wt_page_read(DB *db, WT_PAGE *page);
int
__wt_page_write(DB *db, WT_PAGE *page);
void
__wt_workq_read_server(ENV *env, int force);
int
__wt_cache_read_queue(
    WT_TOC *toc, u_int32_t *addrp, u_int32_t size, WT_PAGE **pagep);
void *
__wt_cache_read_server(void *arg);
int
__wt_cache_sync(
    WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags);
void
__wt_api_db_err(DB *db, int error, const char *fmt, ...);
void
__wt_api_db_errx(DB *db, const char *fmt, ...);
int
__wt_db_btree_compare_int_set_verify(DB *db, int btree_compare_int);
int
__wt_db_btree_dup_offpage_set_verify(DB *db, u_int32_t dup_offpage);
int
__wt_db_column_set_verify(DB *db,
    u_int32_t fixed_len, const char *dictionary, u_int32_t flags);
int
__wt_env_db(ENV *env, DB **dbp);
int
__wt_db_destroy(DB *db);
int
__wt_db_lockout_err(DB *db);
int
__wt_db_lockout_open(DB *db);
int
__wt_db_huffman_set(DB *db,
    u_int8_t const *huffman_table, u_int huffman_table_size, u_int32_t flags);
int
__wt_db_open(WT_TOC *toc, const char *name, mode_t mode, u_int32_t flags);
int
__wt_db_close(WT_TOC *toc, u_int32_t flags);
int
__wt_db_stat_print(WT_TOC *toc, FILE *stream);
int
__wt_db_stat_clear(DB *db);
int
__wt_db_sync(WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags);
void
__wt_api_env_err(ENV *env, int error, const char *fmt, ...);
void
__wt_api_env_errx(ENV *env, const char *fmt, ...);
int
__wt_env_cache_size_set_verify(ENV *env, u_int32_t cache_size);
int
__wt_env_cache_hash_size_set_verify(ENV *env, u_int32_t hash_size);
int
__wt_env_hazard_size_set_verify(ENV *env, u_int32_t hazard_size);
int
__wt_env_toc_size_set_verify(ENV *env, u_int32_t toc_size);
int
__wt_env_verbose_set_verify(ENV *env, u_int32_t verbose);
int
__wt_library_init(void);
int
__wt_breakpoint(void);
void
__wt_attach(ENV *env);
int
__wt_env_create(u_int32_t flags, ENV **envp);
int
__wt_ienv_destroy(ENV *env);
void
__wt_msg(ENV *env, const char *fmt, ...);
void
__wt_mb_init(ENV *env, WT_MBUF *mbp);
void
__wt_mb_discard(WT_MBUF *mbp);
void
__wt_mb_add(WT_MBUF *mbp, const char *fmt, ...);
void
__wt_mb_write(WT_MBUF *mbp);
int
__wt_env_open(ENV *env, const char *home, mode_t mode);
int
__wt_env_close(ENV *env);
int
__wt_env_stat_print(ENV *env, FILE *stream);
int
__wt_env_stat_clear(ENV *env);
void
__wt_stat_print(ENV *env, WT_STATS *s, FILE *stream);
int
__wt_env_sync(ENV *env, void (*f)(const char *, u_int64_t));
int
__wt_env_toc(ENV *env, WT_TOC **tocp);
int
__wt_wt_toc_close(WT_TOC *toc);
int
__wt_toc_api_set(ENV *env, const char *name, DB *db, WT_TOC **tocp);
int
__wt_toc_api_clr(WT_TOC *toc, const char *name, int islocal);
int
__wt_toc_dump(ENV *env);
void *
__wt_workq_srvr(void *arg);
void
__wt_abort(ENV *env);
int
__wt_calloc_func(ENV *env, u_int32_t number, u_int32_t size, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    );
int
__wt_realloc_func(ENV *env,
    u_int32_t *bytes_allocated_ret, u_int32_t bytes_to_allocate, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    );
int
__wt_strdup_func(ENV *env, const char *str, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    );
void
__wt_free_func(ENV *env, void *p_arg
#ifdef HAVE_DIAGNOSTIC
    , u_int32_t len
#endif
    );
int
__wt_mtrack_alloc(ENV *env);
void
__wt_mtrack_free(ENV *env);
void
__wt_mtrack_dump(ENV *env);
int
__wt_filesize(ENV *env, WT_FH *fh, off_t *sizep);
int
__wt_fsync(ENV *env, WT_FH *fh);
int
__wt_mtx_alloc(ENV *env, const char *name, int is_locked, WT_MTX **mtxp);
void
__wt_lock(ENV *env, WT_MTX *mtx);
void
__wt_unlock(ENV *env, WT_MTX *mtx);
int
__wt_mtx_destroy(ENV *env, WT_MTX *mtx);
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
__wt_methods_db_config_default(DB *db);
void
__wt_methods_db_lockout(DB *db);
void
__wt_methods_db_init_transition(DB *db);
void
__wt_methods_db_open_transition(DB *db);
void
__wt_methods_env_config_default(ENV *env);
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
__wt_api_args(ENV *env, const char *name);
int
__wt_api_arg_min(ENV *env,
    const char *name, const char *arg_name, u_int32_t v, u_int32_t min);
int
__wt_api_arg_max(ENV *env,
    const char *name, const char *arg_name, u_int32_t v, u_int32_t max);
int
__wt_database_method_type(DB *db, const char *name, int column_err);
int
__wt_database_wrong_fixed_size(WT_TOC *toc, u_int32_t len);
int
__wt_database_readonly(DB *db, const char *name);
int
__wt_database_format(DB *db);
int
__wt_database_item_too_big(DB *db);
int
__wt_wt_toc_lockout(WT_TOC *toc);
int
__wt_db_lockout(DB *db);
int
__wt_env_lockout(ENV *env);
int
__wt_hazard_set(WT_TOC *toc, WT_CACHE_ENTRY *e, WT_PAGE *page);
void
__wt_hazard_clear(WT_TOC *toc, WT_PAGE *page);
void
__wt_hazard_empty(WT_TOC *toc, const char *name);
int
__wt_huffman_open(ENV *env,
    u_int8_t const *byte_frequency_array, u_int nbytes, void *retp);
void
__wt_huffman_close(ENV *env, void *huffman_arg);
int
__wt_print_huffman_code(ENV *env, void *huffman_arg, u_int16_t symbol);
int
__wt_huffman_encode(void *huffman_arg,
    u_int8_t *from, u_int32_t from_len,
    void *top, u_int32_t *to_len, u_int32_t *out_bytes_used);
int
__wt_huffman_decode(void *huffman_arg,
    u_int8_t *from, u_int32_t from_len,
    void *top, u_int32_t *to_len, u_int32_t *out_bytes_used);
u_int32_t
__wt_nlpo2(u_int32_t v);
int
__wt_ispo2(u_int32_t v);
u_int32_t
__wt_prime(u_int32_t n);
void
__wt_progress(const char *s, u_int64_t v);
int
__wt_toc_scratch_alloc(WT_TOC *toc, DBT **dbtp);
void
__wt_toc_scratch_discard(WT_TOC *toc, DBT *dbt);
void
__wt_toc_scratch_free(WT_TOC *toc);
int
__wt_toc_serialize_func(
    WT_TOC *toc, wq_state_t op, int spin, int (*func)(WT_TOC *), void *args);
void
__wt_toc_serialize_wrapup(WT_TOC *toc, int ret);
int
__wt_stat_alloc_cache_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_cache_stats(WT_STATS *stats);
int
__wt_stat_alloc_database_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_database_stats(WT_STATS *stats);
int
__wt_stat_alloc_db_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_db_stats(WT_STATS *stats);
int
__wt_stat_alloc_env_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_env_stats(WT_STATS *stats);
int
__wt_stat_alloc_fh_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_fh_stats(WT_STATS *stats);
int
__wt_stat_alloc_method_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_method_stats(WT_STATS *stats);
