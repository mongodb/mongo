/* DO NOT EDIT: automatically built by dist/s_prototypes. */
void
__wt_curstd_init(WT_CURSOR_STD *stdc);
void
__wt_curstd_close(WT_CURSOR_STD *c);
int
__wt_block_alloc(WT_TOC *toc, uint32_t *addrp, uint32_t size);
int
__wt_block_free(WT_TOC *toc, uint32_t addr, uint32_t size);
int
__wt_block_read(WT_TOC *toc);
int
__wt_block_write(WT_TOC *toc);
void
__wt_debug_block(WT_TOC *toc);
int
__wt_db_bulk_load(WT_TOC *toc,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **));
int
__wt_item_build_data(WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl);
int
__wt_cache_create(ENV *env);
void
__wt_cache_stats(ENV *env);
int
__wt_cache_destroy(ENV *env);
int
__wt_bt_close(WT_TOC *toc);
int
__wt_bt_lex_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_bt_int_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt);
int
__wt_debug_dump(WT_TOC *toc, const char *ofile, FILE *fp);
int
__wt_debug_disk(WT_TOC *toc, WT_PAGE_DISK *dsk, const char *ofile, FILE *fp);
int
__wt_debug_page(WT_TOC *toc, WT_PAGE *page, const char *ofile, FILE *fp);
void
__wt_debug_dbt(const char *tag, void *arg_dbt, FILE *fp);
int
__wt_desc_stat(WT_TOC *toc);
int
__wt_desc_read(WT_TOC *toc);
int
__wt_desc_write(WT_TOC *toc);
void
__wt_page_discard(WT_TOC *toc, WT_PAGE *page);
int
__wt_db_dump(WT_TOC *toc,
    FILE *stream, void (*f)(const char *, uint64_t), uint32_t flags);
void
__wt_print_byte_string(uint8_t *data, uint32_t size, FILE *stream);
void
__wt_workq_evict_server(ENV *env, int force);
void *
__wt_cache_evict_server(void *arg);
void
__wt_evict_db_clear(WT_TOC *toc);
void
__wt_evict_dump(WT_TOC *toc);
int
__wt_evict_cache_dump(WT_TOC *toc);
int
__wt_evict_tree_dump(WT_TOC *toc, IDB *idb);
int
__wt_evict_cache_count(WT_TOC *toc, uint64_t *nodesp);
int
__wt_evict_tree_count(WT_TOC *toc, IDB *idb, uint64_t *nodesp);
const char *
__wt_page_type_string(WT_PAGE_DISK *dsk);
const char *
__wt_item_type_string(WT_ITEM *item);
int
__wt_bt_open(WT_TOC *toc, int ok_create);
int
__wt_root_pin(WT_TOC *toc);
int
__wt_ovfl_in(WT_TOC *toc, WT_OVFL *ovfl, DBT *store);
int
__wt_page_in(
    WT_TOC *toc, WT_PAGE *parent, WT_REF *ref, void *off, int dsk_verify);
int
__wt_page_inmem(WT_TOC *toc, WT_PAGE *page);
int
__wt_item_process(WT_TOC *toc, WT_ITEM *item, DBT *dbt_ret);
void
__wt_workq_read_server(ENV *env, int force);
int
__wt_cache_read_serial_func(WT_TOC *toc);
void *
__wt_cache_read_server(void *arg);
int
__wt_page_reconcile(WT_TOC *toc, WT_PAGE *page);
int
__wt_rle_expand_sort(ENV *env,
    WT_PAGE *page, WT_COL *cip, WT_RLE_EXPAND ***expsortp, uint32_t *np);
int
__wt_dbt_return(WT_TOC *toc, DBT *key, DBT *data, int key_return);
int
__wt_page_disk_read(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_page_disk_write(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_page_stat(WT_TOC *toc, WT_PAGE *page, void *arg);
int
__wt_bt_sync(WT_TOC *toc);
int
__wt_db_verify(WT_TOC *toc, void (*f)(const char *, uint64_t));
int
__wt_verify(
    WT_TOC *toc, void (*f)(const char *, uint64_t), FILE *stream);
int
__wt_verify_dsk_page(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_verify_dsk_chunk(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_tree_walk(WT_TOC *toc, WT_REF *ref,
    uint32_t flags, int (*work)(WT_TOC *, WT_PAGE *, void *), void *arg);
int
__wt_walk_begin(WT_TOC *toc, WT_REF *ref, WT_WALK *walk);
void
__wt_walk_end(ENV *env, WT_WALK *walk);
int
__wt_walk_next(WT_TOC *toc, WT_WALK *walk, WT_REF **refp);
int
__wt_db_col_get(WT_TOC *toc, uint64_t recno, DBT *data);
int
__wt_db_col_del(WT_TOC *toc, uint64_t recno);
int
__wt_db_col_put(WT_TOC *toc, uint64_t recno, DBT *data);
int
__wt_rle_expand_serial_func(WT_TOC *toc);
int
__wt_rle_expand_repl_serial_func(WT_TOC *toc);
int
__wt_col_search(WT_TOC *toc, uint64_t recno, uint32_t level, uint32_t flags);
int
__wt_db_row_get(WT_TOC *toc, DBT *key, DBT *data);
int
__wt_db_row_del(WT_TOC *toc, DBT *key);
int
__wt_db_row_put(WT_TOC *toc, DBT *key, DBT *data);
int
__wt_item_update_serial_func(WT_TOC *toc);
int
__wt_repl_alloc(WT_TOC *toc, WT_REPL **replp, DBT *data);
void
__wt_repl_free(WT_TOC *toc, WT_REPL *repl);
int
__wt_row_search(WT_TOC *toc, DBT *key, uint32_t level, uint32_t flags);
void
__wt_api_db_err(DB *db, int error, const char *fmt, ...);
void
__wt_api_db_errx(DB *db, const char *fmt, ...);
int
__wt_db_btree_compare_int_set_verify(DB *db, int btree_compare_int);
int
__wt_db_column_set_verify(
    DB *db, uint32_t fixed_len, const char *dictionary, uint32_t flags);
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
    uint8_t const *huffman_table, u_int huffman_table_size, uint32_t flags);
int
__wt_db_open(WT_TOC *toc, const char *name, mode_t mode, uint32_t flags);
int
__wt_db_close(WT_TOC *toc, uint32_t flags);
int
__wt_db_stat_print(WT_TOC *toc, FILE *stream);
int
__wt_db_stat_clear(DB *db);
int
__wt_db_sync(WT_TOC *toc, void (*f)(const char *, uint64_t), uint32_t flags);
void
__wt_api_env_err(ENV *env, int error, const char *fmt, ...);
void
__wt_api_env_errx(ENV *env, const char *fmt, ...);
int
__wt_env_cache_size_set_verify(ENV *env, uint32_t cache_size);
int
__wt_env_cache_hash_size_set_verify(ENV *env, uint32_t hash_size);
int
__wt_env_hazard_size_set_verify(ENV *env, uint32_t hazard_size);
int
__wt_env_toc_size_set_verify(ENV *env, uint32_t toc_size);
int
__wt_env_verbose_set_verify(ENV *env, uint32_t verbose);
int
__wt_library_init(void);
int
__wt_breakpoint(void);
void
__wt_attach(ENV *env);
int
__wt_env_create(uint32_t flags, ENV **envp);
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
__wt_env_sync(ENV *env, void (*f)(const char *, uint64_t));
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
__wt_calloc_func(ENV *env, size_t number, size_t size, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    );
int
__wt_realloc_func(ENV *env,
    uint32_t *bytes_allocated_ret, size_t bytes_to_allocate, void *retp
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
    , size_t len, const char *file, int line
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
__wt_ftruncate(WT_TOC *toc, WT_FH *fh, off_t len);
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
__wt_read(ENV *env, WT_FH *fh, off_t offset, uint32_t bytes, void *buf);
int
__wt_write(ENV *env, WT_FH *fh, off_t offset, uint32_t bytes, void *buf);
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
uint32_t
__wt_cksum(void *chunk, uint32_t bytes);
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
    const char *name, const char *arg_name, uint32_t v, uint32_t min);
int
__wt_api_arg_max(ENV *env,
    const char *name, const char *arg_name, uint32_t v, uint32_t max);
int
__wt_file_method_type(DB *db, const char *name, int column_err);
int
__wt_file_wrong_fixed_size(WT_TOC *toc, uint32_t len);
int
__wt_file_readonly(DB *db, const char *name);
int
__wt_file_format(DB *db);
int
__wt_file_item_too_big(DB *db);
int
__wt_wt_toc_lockout(WT_TOC *toc);
int
__wt_db_lockout(DB *db);
int
__wt_env_lockout(ENV *env);
int
__wt_hazard_set(WT_TOC *toc, WT_REF *ref);
void
__wt_hazard_clear(WT_TOC *toc, WT_PAGE *page);
void
__wt_hazard_empty(WT_TOC *toc, const char *name);
int
__wt_huffman_open(ENV *env,
    uint8_t const *byte_frequency_array, u_int nbytes, void *retp);
void
__wt_huffman_close(ENV *env, void *huffman_arg);
int
__wt_print_huffman_code(ENV *env, void *huffman_arg, uint16_t symbol);
int
__wt_huffman_encode(void *huffman_arg,
    uint8_t *from, uint32_t from_len,
    void *top, uint32_t *to_len, uint32_t *out_bytes_used);
int
__wt_huffman_decode(void *huffman_arg,
    uint8_t *from, uint32_t from_len,
    void *top, uint32_t *to_len, uint32_t *out_bytes_used);
uint32_t
__wt_nlpo2(uint32_t v);
int
__wt_ispo2(uint32_t v);
uint32_t
__wt_prime(uint32_t n);
void
__wt_progress(const char *s, uint64_t v);
int
__wt_scr_alloc(WT_TOC *toc, uint32_t size, DBT **dbtp);
void
__wt_scr_release(DBT **dbt);
void
__wt_scr_free(WT_TOC *toc);
int
__wt_toc_serialize_func(
    WT_TOC *toc, wq_state_t op, int spin, int (*func)(WT_TOC *), void *args);
void
__wt_toc_serialize_wrapup(WT_TOC *toc, WT_PAGE *page, int ret);
int
__wt_stat_alloc_cache_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_cache_stats(WT_STATS *stats);
int
__wt_stat_alloc_file_stats(ENV *env, WT_STATS **statsp);
void
__wt_stat_clear_file_stats(WT_STATS *stats);
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
