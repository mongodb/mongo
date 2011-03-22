/* DO NOT EDIT: automatically built by dist/s_prototypes. */
void
__wt_methods_btree_config_default(BTREE *btree);
void
__wt_methods_btree_lockout(BTREE *btree);
void
__wt_methods_btree_init_transition(BTREE *btree);
void
__wt_methods_btree_open_transition(BTREE *btree);
void
__wt_methods_connection_config_default(CONNECTION *connection);
void
__wt_methods_connection_lockout(CONNECTION *connection);
void
__wt_methods_connection_open_transition(CONNECTION *connection);
void
__wt_methods_connection_init_transition(CONNECTION *connection);
void
__wt_methods_session_lockout(SESSION *session);
void
__wt_methods_session_init_transition(SESSION *session);
int
__wt_config_init(WT_CONFIG *conf, const char *str, size_t len);
int
__wt_config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value);
int
__wt_session_add_btree(SESSION *session, BTREE *btree);
int
__wt_cursor_open(SESSION *session,
    const char *uri, const char *config, WT_CURSOR **cursorp);
int
__wt_curbulk_init(CURSOR_BULK *cbulk);
void
__wt_curdump_init(WT_CURSOR *cursor);
int
__wt_cursor_close(WT_CURSOR *cursor, const char *config);
void
__wt_cursor_init(WT_CURSOR *cursor);
int
__wt_block_alloc(SESSION *session, uint32_t *addrp, uint32_t size);
int
__wt_block_free(SESSION *session, uint32_t addr, uint32_t size);
int
__wt_block_read(SESSION *session);
int
__wt_block_write(SESSION *session);
void
__wt_debug_block(SESSION *session);
int
__wt_btree_bulk_load(SESSION *session,
    void (*f)(const char *, uint64_t),
    int (*cb)(BTREE *, WT_ITEM **, WT_ITEM **));
int
__wt_bulk_init(CURSOR_BULK *cbulk);
int
__wt_bulk_var_insert(CURSOR_BULK *cbulk);
int
__wt_bulk_end(CURSOR_BULK *cbulk);
int
__wt_item_build_key(
    SESSION *session, WT_ITEM *item, WT_CELL *cell, WT_OVFL *ovfl);
int
__wt_item_build_value(SESSION *session,
    WT_ITEM *item, WT_CELL *cell, WT_OVFL *ovfl);
int
__wt_cache_create(CONNECTION *conn);
void
__wt_cache_stats(CONNECTION *conn);
int
__wt_cache_destroy(CONNECTION *conn);
int
__wt_bt_close(SESSION *session);
int
__wt_bt_lex_compare(
    BTREE *btree, const WT_ITEM *user_item, const WT_ITEM *tree_item);
int
__wt_bt_int_compare(
    BTREE *btree, const WT_ITEM *user_item, const WT_ITEM *tree_item);
int
__wt_btcur_first(CURSOR_BTREE *cbt);
int
__wt_btcur_next(CURSOR_BTREE *cbt);
int
__wt_btcur_prev(CURSOR_BTREE *cbt);
int
__wt_btcur_search_near(CURSOR_BTREE *cbt, int *exact);
int
__wt_btcur_insert(CURSOR_BTREE *cbt);
int
__wt_btcur_update(CURSOR_BTREE *cbt);
int
__wt_btcur_remove(CURSOR_BTREE *cbt);
int
__wt_btcur_close(CURSOR_BTREE *cbt, const char *config);
int
__wt_debug_dump(SESSION *session, const char *ofile, FILE *fp);
int
__wt_debug_disk(
    SESSION *session, WT_PAGE_DISK *dsk, const char *ofile, FILE *fp);
int
__wt_debug_page(SESSION *session, WT_PAGE *page, const char *ofile, FILE *fp);
void
__wt_debug_item(const char *tag, void *arg_item, FILE *fp);
int
__wt_desc_stat(SESSION *session);
int
__wt_desc_read(SESSION *session);
int
__wt_desc_write(SESSION *session);
void
__wt_page_discard(SESSION *session, WT_PAGE *page);
int
__wt_btree_dump(SESSION *session,
    FILE *stream, void (*f)(const char *, uint64_t), uint32_t flags);
void
__wt_print_byte_string(const uint8_t *data, uint32_t size, FILE *stream);
void
__wt_workq_evict_server(CONNECTION *conn, int force);
void *
__wt_cache_evict_server(void *arg);
void
__wt_evict_db_clear(SESSION *session);
void
__wt_evict_dump(SESSION *session);
int
__wt_evict_cache_dump(SESSION *session);
int
__wt_evict_tree_dump(SESSION *session, BTREE *btree);
int
__wt_evict_cache_count(SESSION *session, uint64_t *nodesp);
int
__wt_evict_tree_count(SESSION *session, BTREE *btree, uint64_t *nodesp);
const char *
__wt_page_type_string(u_int type);
const char *
__wt_cell_type_string(WT_CELL *cell);
int
__wt_bt_open(SESSION *session, int ok_create);
int
__wt_root_pin(SESSION *session);
int
__wt_ovfl_in(SESSION *session, WT_OVFL *ovfl, WT_BUF *store);
int
__wt_page_in(SESSION *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify);
int
__wt_page_inmem(SESSION *session, WT_PAGE *page);
int
__wt_cell_process(SESSION *session, WT_CELL *cell, WT_BUF *scratch);
void
__wt_workq_read_server(CONNECTION *conn, int force);
int
__wt_cache_read_serial_func(SESSION *session);
void *
__wt_cache_read_server(void *arg);
int
__wt_page_reconcile(SESSION *session, WT_PAGE *page, int discard);
int
__wt_rle_expand_sort(SESSION *session,
    WT_PAGE *page, WT_COL *cip, WT_RLE_EXPAND ***expsortp, WT_BUF **tmpp);
int
__wt_value_return(
    SESSION *session, WT_ITEM *key, WT_ITEM *value, int key_return);
int
__wt_disk_read(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_disk_write(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_page_stat(SESSION *session, WT_PAGE *page, void *arg);
int
__wt_bt_sync(SESSION *session);
int
__wt_btree_verify(SESSION *session, void (*f)(const char *, uint64_t));
int
__wt_verify(SESSION *session, void (*f)(const char *, uint64_t), FILE *stream);
int
__wt_verify_dsk_page(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_verify_dsk_chunk(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size);
int
__wt_tree_walk(SESSION *session, WT_PAGE *page,
    uint32_t flags, int (*work)(SESSION *, WT_PAGE *, void *), void *arg);
int
__wt_walk_begin(SESSION *session, WT_REF *ref, WT_WALK *walk);
void
__wt_walk_end(SESSION *session, WT_WALK *walk);
int
__wt_walk_next(SESSION *session, WT_WALK *walk, uint32_t flags, WT_REF **refp);
int
__wt_btree_col_get(SESSION *session, uint64_t recno, WT_ITEM *value);
int
__wt_btree_col_del(SESSION *session, uint64_t recno);
int
__wt_btree_col_put(SESSION *session, uint64_t recno, WT_ITEM *value);
int
__wt_rle_expand_serial_func(SESSION *session);
int
__wt_rle_expand_update_serial_func(SESSION *session);
int
__wt_col_search(SESSION *session, uint64_t recno, uint32_t flags);
int
__wt_btree_row_get(SESSION *session, WT_ITEM *key, WT_ITEM *value);
int
__wt_btree_row_del(SESSION *session, WT_ITEM *key);
int
__wt_btree_row_put(SESSION *session, WT_ITEM *key, WT_ITEM *value);
int
__wt_item_update_serial_func(SESSION *session);
int
__wt_update_alloc(SESSION *session, WT_UPDATE **updp, WT_ITEM *value);
void
__wt_update_free(SESSION *session, WT_UPDATE *upd);
int
__wt_row_search(SESSION *session, WT_ITEM *key, uint32_t flags);
int
__wt_btree_btree_compare_int_set_verify(BTREE *btree, int btree_compare_int);
int
__wt_btree_column_set_verify(
    BTREE *btree, uint32_t fixed_len, const char *dictionary, uint32_t flags);
int
__wt_connection_btree(CONNECTION *conn, BTREE **btreep);
int
__wt_btree_destroy(BTREE *btree);
int
__wt_btree_lockout_err(BTREE *btree);
int
__wt_btree_lockout_open(BTREE *btree);
int
__wt_btree_huffman_set(BTREE *btree,
    uint8_t const *huffman_table, u_int huffman_table_size, uint32_t flags);
int
__wt_btree_open(SESSION *session,
    const char *name,
    mode_t mode,
    uint32_t flags);
int
__wt_btree_close(SESSION *session, uint32_t flags);
int
__wt_btree_stat_print(SESSION *session, FILE *stream);
int
__wt_btree_stat_clear(BTREE *btree);
int
__wt_btree_sync(SESSION *session,
    void (*f)(const char *, uint64_t),
    uint32_t flags);
int
__wt_connection_cache_size_set_verify(CONNECTION *conn, uint32_t cache_size);
int
__wt_connection_cache_hash_size_set_verify(CONNECTION *conn,
    uint32_t hash_size);
int
__wt_connection_hazard_size_set_verify(CONNECTION *conn, uint32_t hazard_size);
int
__wt_connection_session_size_set_verify(CONNECTION *conn, uint32_t toc_size);
int
__wt_connection_verbose_set_verify(CONNECTION *conn, uint32_t verbose);
int
__wt_library_init(void);
int
__wt_breakpoint(void);
void
__wt_attach(SESSION *session);
int
__wt_connection_config(CONNECTION *conn);
int
__wt_connection_destroy(CONNECTION *conn);
void
__wt_msg(SESSION *session, const char *fmt, ...);
void
__wt_mb_init(SESSION *session, WT_MBUF *mbp);
void
__wt_mb_discard(WT_MBUF *mbp);
void
__wt_mb_add(WT_MBUF *mbp, const char *fmt, ...);
void
__wt_mb_write(WT_MBUF *mbp);
int
__wt_connection_open(CONNECTION *conn, const char *home, mode_t mode);
int
__wt_connection_close(CONNECTION *conn);
int
__wt_connection_session(CONNECTION *conn, SESSION **sessionp);
int
__wt_session_close(SESSION *session);
int
__wt_session_api_set(
    CONNECTION *conn, const char *name, BTREE *btree, SESSION **sessionp);
int
__wt_session_api_clr(SESSION *session, const char *name, int islocal);
int
__wt_session_dump(CONNECTION *conn);
int
__wt_connection_stat_print(CONNECTION *conn, FILE *stream);
int
__wt_connection_stat_clear(CONNECTION *conn);
void
__wt_stat_print(CONNECTION *conn, WT_STATS *s, FILE *stream);
int
__wt_connection_sync(CONNECTION *conn, void (*f)(const char *, uint64_t));
void *
__wt_workq_srvr(void *arg);
void
__wt_abort(SESSION *session);
int
__wt_calloc(SESSION *session, size_t number, size_t size, void *retp);
int
__wt_realloc(SESSION *session,
    uint32_t *bytes_allocated_ret, size_t bytes_to_allocate, void *retp);
int
__wt_strdup(SESSION *session, const char *str, void *retp);
void
__wt_free_int(SESSION *session, void *p_arg);
int
__wt_filesize(SESSION *session, WT_FH *fh, off_t *sizep);
int
__wt_fsync(SESSION *session, WT_FH *fh);
int
__wt_ftruncate(SESSION *session, WT_FH *fh, off_t len);
int
__wt_mtx_alloc(SESSION *session,
    const char *name,
    int is_locked,
    WT_MTX **mtxp);
void
__wt_lock(SESSION *session, WT_MTX *mtx);
void
__wt_unlock(SESSION *session, WT_MTX *mtx);
int
__wt_mtx_destroy(SESSION *session, WT_MTX *mtx);
int
__wt_open(
    SESSION *session,
    const char *name,
    mode_t mode,
    int ok_create,
    WT_FH **fhp);
int
__wt_close(SESSION *session, WT_FH *fh);
int
__wt_read(SESSION *session, WT_FH *fh, off_t offset, uint32_t bytes, void *buf);
int
__wt_write(SESSION *session,
    WT_FH *fh,
    off_t offset,
    uint32_t bytes,
    void *buf);
void
__wt_sleep(long seconds, long micro_seconds);
int
__wt_thread_create(pthread_t *tidret, void *(*func)(void *), void *arg);
void
__wt_thread_join(pthread_t tid);
void
__wt_yield(void);
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
__wt_assert(
    SESSION *session,
    const char *check,
    const char *file_name,
    int line_number);
int
__wt_api_args(SESSION *session, const char *name);
int
__wt_api_arg_min(SESSION *session,
    const char *name, const char *arg_name, uint32_t v, uint32_t min);
int
__wt_api_arg_max(SESSION *session,
    const char *name, const char *arg_name, uint32_t v, uint32_t max);
int
__wt_file_method_type(SESSION *session, const char *name, int column_err);
int
__wt_file_wrong_fixed_size(SESSION *session, uint32_t len, uint32_t config_len);
int
__wt_file_readonly(SESSION *session, const char *name);
int
__wt_file_format(SESSION *session);
int
__wt_file_item_too_big(SESSION *session);
int
__wt_session_lockout(SESSION *session);
int
__wt_btree_lockout(BTREE *btree);
int
__wt_connection_lockout(CONNECTION *conn);
void
__wt_errv(SESSION *session, int error,
    const char *prefix, const char *fmt, va_list ap);
void
__wt_err(SESSION *session, int error, const char *fmt, ...);
void
__wt_errx(SESSION *session, const char *fmt, ...);
int
__wt_hazard_set(SESSION *session, WT_REF *ref);
void
__wt_hazard_clear(SESSION *session, WT_PAGE *page);
void
__wt_hazard_empty(SESSION *session, const char *name);
int
__wt_huffman_open(SESSION *session,
    uint8_t const *byte_frequency_array, u_int nbytes, void *retp);
void
__wt_huffman_close(SESSION *session, void *huffman_arg);
int
__wt_print_huffman_code(SESSION *session, void *huffman_arg, uint16_t symbol);
int
__wt_huffman_encode(void *huffman_arg,
    const uint8_t *from, uint32_t from_len, WT_BUF *to_buf);
int
__wt_huffman_decode(void *huffman_arg,
    const uint8_t *from, uint32_t from_len, WT_BUF *to_buf);
uint32_t
__wt_nlpo2(uint32_t v);
int
__wt_ispo2(uint32_t v);
uint32_t
__wt_prime(uint32_t n);
void
__wt_progress(const char *s, uint64_t v);
int
__wt_buf_grow(SESSION *session, WT_BUF *buf, size_t sz);
void
__wt_buf_free(SESSION *session, WT_BUF *buf);
int
__wt_scr_alloc(SESSION *session, uint32_t size, WT_BUF **scratchp);
void
__wt_scr_release(WT_BUF **bufp);
void
__wt_scr_free(SESSION *session);
int
__wt_session_serialize_func(SESSION *session,
    wq_state_t op, int spin, int (*func)(SESSION *), void *args);
void
__wt_session_serialize_wrapup(SESSION *session, WT_PAGE *page, int ret);
int
__wt_stat_alloc_btree_handle_stats(SESSION *session, WT_STATS **statsp);
void
__wt_stat_clear_btree_handle_stats(WT_STATS *stats);
int
__wt_stat_alloc_btree_file_stats(SESSION *session, WT_STATS **statsp);
void
__wt_stat_clear_btree_file_stats(WT_STATS *stats);
int
__wt_stat_alloc_cache_stats(SESSION *session, WT_STATS **statsp);
void
__wt_stat_clear_cache_stats(WT_STATS *stats);
int
__wt_stat_alloc_connection_stats(SESSION *session, WT_STATS **statsp);
void
__wt_stat_clear_connection_stats(WT_STATS *stats);
int
__wt_stat_alloc_fh_stats(SESSION *session, WT_STATS **statsp);
void
__wt_stat_clear_fh_stats(WT_STATS *stats);
int
__wt_stat_alloc_method_stats(SESSION *session, WT_STATS **statsp);
void
__wt_stat_clear_method_stats(WT_STATS *stats);
