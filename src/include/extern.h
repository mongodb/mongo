/* DO NOT EDIT: automatically built by dist/s_prototypes. */

#ifdef __GNUC__
#define	WT_GCC_ATTRIBUTE(x)	__attribute__(x)
#else
#define	WT_GCC_ATTRIBUTE(x)
#endif

extern int __wt_config_initn(WT_CONFIG *conf, const char *str, size_t len);
extern int __wt_config_init(WT_CONFIG *conf, const char *str);
extern int __wt_config_next(WT_CONFIG *conf,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_getraw( WT_CONFIG *cparser,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_get(const char **cfg,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_gets(const char **cfg,
    const char *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_getone(const char *cfg,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_getones(const char *cfg,
    const char *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_checklist(WT_SESSION_IMPL *session,
    const char **defaults,
    const char *config);
extern int __wt_config_check( WT_SESSION_IMPL *session,
    const char *defaults,
    const char *config);
extern int __wt_config_collapse(WT_SESSION_IMPL *session,
    const char **cfg,
    const char **config_ret);
extern const char *__wt_confdfl_connection_add_collator;
extern const char *__wt_confdfl_connection_add_compressor;
extern const char *__wt_confdfl_connection_add_cursor_type;
extern const char *__wt_confdfl_connection_add_extractor;
extern const char *__wt_confdfl_connection_close;
extern const char *__wt_confdfl_connection_load_extension;
extern const char *__wt_confdfl_connection_open_session;
extern const char *__wt_confdfl_cursor_close;
extern const char *__wt_confdfl_session_begin_transaction;
extern const char *__wt_confdfl_session_checkpoint;
extern const char *__wt_confdfl_session_close;
extern const char *__wt_confdfl_session_commit_transaction;
extern const char *__wt_confdfl_session_create;
extern const char *__wt_confdfl_session_drop;
extern const char *__wt_confdfl_session_log_printf;
extern const char *__wt_confdfl_session_open_cursor;
extern const char *__wt_confdfl_session_rename;
extern const char *__wt_confdfl_session_rollback_transaction;
extern const char *__wt_confdfl_session_salvage;
extern const char *__wt_confdfl_session_sync;
extern const char *__wt_confdfl_session_truncate;
extern const char *__wt_confdfl_session_verify;
extern const char *__wt_confdfl_wiredtiger_open;
extern int __wt_curbtree_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_curbulk_init(CURSOR_BULK *cbulk);
extern int __wt_curconfig_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
extern void __wt_curdump_init(WT_CURSOR *cursor, int printable);
extern int __wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_cursor_close(WT_CURSOR *cursor, const char *config);
extern void __wt_cursor_init(WT_CURSOR *cursor, const char *config);
extern int __wt_session_add_btree( WT_SESSION_IMPL *session,
    WT_BTREE_SESSION **btree_sessionp);
extern int __wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_BTREE_SESSION **btree_sessionp);
extern int __wt_session_remove_btree( WT_SESSION_IMPL *session,
    WT_BTREE_SESSION *btree_session);
extern int __wt_block_alloc(WT_SESSION_IMPL *session,
    uint32_t *addrp,
    uint32_t size);
extern int __wt_block_free(WT_SESSION_IMPL *session,
    uint32_t addr,
    uint32_t size);
extern int __wt_block_read(WT_SESSION_IMPL *session);
extern int __wt_block_write(WT_SESSION_IMPL *session);
extern void __wt_block_discard(WT_SESSION_IMPL *session);
extern void __wt_block_dump(WT_SESSION_IMPL *session);
extern int __wt_bulk_init(CURSOR_BULK *cbulk);
extern int __wt_bulk_insert(CURSOR_BULK *cbulk);
extern int __wt_bulk_end(CURSOR_BULK *cbulk);
extern int __wt_cache_create(WT_CONNECTION_IMPL *conn);
extern void __wt_cache_stats_update(WT_CONNECTION_IMPL *conn);
extern void __wt_cache_destroy(WT_CONNECTION_IMPL *conn);
extern void __wt_cell_set(WT_CELL *cell,
    u_int type,
    u_int prefix,
    uint32_t size,
    uint32_t *cell_lenp);
extern void *__wt_cell_data(WT_CELL *cell);
extern uint32_t __wt_cell_datalen(WT_CELL *cell);
extern uint32_t __wt_cell_len(WT_CELL *cell);
extern int __wt_cell_copy(WT_SESSION_IMPL *session,
    WT_CELL *cell,
    WT_BUF *retb);
extern int __wt_bt_lex_compare( WT_BTREE *btree,
    const WT_ITEM *user_item,
    const WT_ITEM *tree_item);
extern int __wt_btcur_first(CURSOR_BTREE *cbt);
extern int __wt_btcur_next(CURSOR_BTREE *cbt);
extern int __wt_btcur_prev(CURSOR_BTREE *cbt);
extern int __wt_btcur_search_near(CURSOR_BTREE *cbt, int *exact);
extern int __wt_btcur_insert(CURSOR_BTREE *cbt);
extern int __wt_btcur_update(CURSOR_BTREE *cbt);
extern int __wt_btcur_remove(CURSOR_BTREE *cbt);
extern int __wt_btcur_close(CURSOR_BTREE *cbt, const char *config);
extern int __wt_debug_dump(WT_SESSION_IMPL *session,
    const char *ofile,
    FILE *fp);
extern int __wt_debug_addr(WT_SESSION_IMPL *session,
    uint32_t addr,
    uint32_t size,
    const char *ofile,
    FILE *fp);
extern int __wt_debug_disk( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    const char *ofile,
    FILE *fp);
extern int __wt_debug_tree_all( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const char *ofile,
    FILE *fp);
extern int __wt_debug_tree( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const char *ofile,
    FILE *fp);
extern int __wt_debug_page( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const char *ofile,
    FILE *fp);
extern void __wt_debug_pair(const char *tag,
    const void *data,
    uint32_t size,
    FILE *fp);
extern int __wt_desc_read(WT_SESSION_IMPL *session);
extern int __wt_desc_write(WT_SESSION_IMPL *session,
    const char *config,
    WT_FH *fh);
extern int __wt_desc_update(WT_SESSION_IMPL *session);
extern void __wt_page_free( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    uint32_t addr,
    uint32_t size);
extern int __wt_btree_dump(WT_SESSION_IMPL *session,
    FILE *stream,
    uint32_t flags);
extern void __wt_print_byte_string(const uint8_t *data,
    uint32_t size,
    FILE *stream);
extern void __wt_workq_evict_server(WT_CONNECTION_IMPL *conn, int force);
extern int __wt_evict_file_serial_func(WT_SESSION_IMPL *session);
extern void *__wt_cache_evict_server(void *arg);
extern void __wt_workq_evict_server_exit(WT_CONNECTION_IMPL *conn);
extern int __wt_btree_create( WT_SESSION_IMPL *session,
    const char *name,
    const char *config);
extern int __wt_btree_open(WT_SESSION_IMPL *session,
    const char *name,
    uint32_t flags);
extern int __wt_btree_close(WT_SESSION_IMPL *session);
extern int __wt_btree_huffman_open(WT_SESSION_IMPL *session);
extern void __wt_btree_huffman_close(WT_SESSION_IMPL *session);
extern const char *__wt_page_type_string(u_int type);
extern const char *__wt_cell_type_string(WT_CELL *cell);
extern int __wt_ovfl_in(WT_SESSION_IMPL *session, WT_OFF *ovfl, WT_BUF *store);
extern int
__wt_page_in_func(
 WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, int dsk_verify
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern int __wt_page_inmem(WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *parent_ref,
    WT_PAGE_DISK *dsk,
    WT_PAGE **pagep);
extern void __wt_workq_read_server(WT_CONNECTION_IMPL *conn, int force);
extern int __wt_cache_read_serial_func(WT_SESSION_IMPL *session);
extern void *__wt_cache_read_server(void *arg);
extern void __wt_workq_read_server_exit(WT_CONNECTION_IMPL *conn);
extern void __wt_rec_destroy(WT_SESSION_IMPL *session);
extern int __wt_page_reconcile( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    uint32_t slvg_skip,
    uint32_t flags);
extern int __wt_return_data( WT_SESSION_IMPL *session,
    WT_ITEM *key,
    WT_ITEM *value,
    int key_return);
extern int __wt_disk_read( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size);
extern int __wt_disk_write( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size);
extern int __wt_salvage(WT_SESSION_IMPL *session, const char *config);
extern void __wt_trk_dump(const char *l, void *ss_arg);
extern int __wt_page_stat(WT_SESSION_IMPL *session, WT_PAGE *page, void *arg);
extern int __wt_bt_sync(WT_SESSION_IMPL *session);
extern int __wt_verify(WT_SESSION_IMPL *session,
    FILE *stream,
    const char *config);
extern int __wt_verify_dsk_page( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size);
extern int __wt_verify_dsk_chunk( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size);
extern int __wt_tree_walk(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    int (*work)(WT_SESSION_IMPL *,
    WT_PAGE *,
    void *),
    void *arg);
extern int __wt_walk_begin( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_WALK *walk,
    uint32_t flags);
extern void __wt_walk_end(WT_SESSION_IMPL *session, WT_WALK *walk);
extern int __wt_walk_next(WT_SESSION_IMPL *session,
    WT_WALK *walk,
    WT_PAGE **pagep);
extern int __wt_btree_col_get(WT_SESSION_IMPL *session,
    uint64_t recno,
    WT_ITEM *value);
extern int __wt_btree_col_del(WT_SESSION_IMPL *session, uint64_t recno);
extern int __wt_btree_col_put(WT_SESSION_IMPL *session,
    uint64_t recno,
    WT_ITEM *value);
extern int __wt_col_search(WT_SESSION_IMPL *session,
    uint64_t recno,
    uint32_t flags);
extern int __wt_btree_row_get(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    WT_ITEM *value);
extern int __wt_row_key( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_ROW *rip_arg,
    WT_BUF *retb);
extern WT_CELL *__wt_row_value(WT_PAGE *page, WT_ROW *rip);
extern int __wt_row_ikey_alloc(WT_SESSION_IMPL *session,
    uint32_t cell_offset,
    const void *key,
    uint32_t size,
    WT_IKEY **ikeyp);
extern int __wt_row_key_serial_func(WT_SESSION_IMPL *session);
extern int __wt_btree_row_del(WT_SESSION_IMPL *session, WT_ITEM *key);
extern int __wt_btree_row_put(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    WT_ITEM *value);
extern int __wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    WT_INSERT **insp);
extern int __wt_insert_serial_func(WT_SESSION_IMPL *session);
extern int __wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value,
    WT_UPDATE **updp);
extern int __wt_update_serial_func(WT_SESSION_IMPL *session);
extern int __wt_row_search(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    uint32_t flags);
extern int __wt_btree_stat_print(WT_SESSION_IMPL *session, FILE *stream);
extern int __wt_btree_stat_clear(WT_BTREE *btree);
extern int __wt_library_init(void);
extern int __wt_breakpoint(void);
extern void __wt_attach(WT_SESSION_IMPL *session);
extern int __wt_connection_config(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_destroy(WT_CONNECTION_IMPL *conn);
extern void __wt_mb_init(WT_SESSION_IMPL *session, WT_MBUF *mbp);
extern void __wt_mb_discard(WT_MBUF *mbp);
extern void __wt_mb_add(WT_MBUF *mbp, const char *fmt, ...);
extern void __wt_mb_write(WT_MBUF *mbp);
extern int __wt_connection_open(WT_CONNECTION_IMPL *conn,
    const char *home,
    mode_t mode);
extern int __wt_connection_close(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_session(WT_CONNECTION_IMPL *conn,
    WT_SESSION_IMPL **sessionp);
extern int __wt_session_close(WT_SESSION_IMPL *session);
extern void __wt_session_dump(WT_SESSION_IMPL *session);
extern int __wt_connection_stat_print(WT_CONNECTION_IMPL *conn, FILE *stream);
extern int __wt_connection_stat_clear(WT_CONNECTION_IMPL *conn);
extern void __wt_stat_print(WT_STATS *s, FILE *stream);
extern void *__wt_workq_srvr(void *arg);
extern int __wt_log_put(WT_SESSION_IMPL *session, WT_LOGREC_DESC *recdesc, ...);
extern int __wt_log_vprintf(WT_SESSION_IMPL *session,
    const char *fmt,
    va_list ap);
extern int __wt_log_printf(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    2,
    3)));
extern WT_LOGREC_DESC __wt_logdesc_debug;
extern void __wt_abort(WT_SESSION_IMPL *session);
extern int __wt_calloc(WT_SESSION_IMPL *session,
    size_t number,
    size_t size,
    void *retp);
extern int __wt_realloc(WT_SESSION_IMPL *session,
    uint32_t *bytes_allocated_ret,
    size_t bytes_to_allocate,
    void *retp);
extern int __wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp);
extern void __wt_free_int(WT_SESSION_IMPL *session, void *p_arg);
extern int __wt_dlopen(WT_SESSION_IMPL *session,
    const char *path,
    WT_DLH **dlhp);
extern int __wt_dlsym(WT_SESSION_IMPL *session,
    WT_DLH *dlh,
    const char *name,
    void **sym_ret);
extern int __wt_dlclose(WT_SESSION_IMPL *session, WT_DLH *dlh);
extern int __wt_exist(const char *path);
extern int __wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, off_t *sizep);
extern int __wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh);
extern int __wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, off_t len);
extern int __wt_mtx_alloc(WT_SESSION_IMPL *session,
    const char *name,
    int is_locked,
    WT_MTX **mtxp);
extern void __wt_lock(WT_SESSION_IMPL *session, WT_MTX *mtx);
extern void __wt_unlock(WT_SESSION_IMPL *session, WT_MTX *mtx);
extern int __wt_mtx_destroy(WT_SESSION_IMPL *session, WT_MTX *mtx);
extern int __wt_open(WT_SESSION_IMPL *session,
    const char *name,
    mode_t mode,
    int ok_create,
    WT_FH **fhp);
extern int __wt_close(WT_SESSION_IMPL *session, WT_FH *fh);
extern int __wt_read(WT_SESSION_IMPL *session,
    WT_FH *fh,
    off_t offset,
    uint32_t bytes,
    void *buf);
extern int __wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh,
    off_t offset,
    uint32_t bytes,
    void *buf);
extern void __wt_sleep(long seconds, long micro_seconds);
extern int __wt_thread_create(pthread_t *tidret,
    void *(*func)(void *),
    void *arg);
extern void __wt_thread_join(pthread_t tid);
extern void __wt_yield(void);
extern uint32_t __wt_cksum(const void *chunk, size_t len);
extern void __wt_errv(WT_SESSION_IMPL *session,
    int error,
    const char *prefix1,
    const char *prefix2,
    const char *fmt,
    va_list ap);
extern void __wt_err(WT_SESSION_IMPL *session,
    int error,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    3,
    4)));
extern void __wt_errx(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    2,
    3)));
extern void __wt_msgv(WT_SESSION_IMPL *session,
    const char *prefix1,
    const char *prefix2,
    const char *fmt,
    va_list ap);
extern void __wt_msg(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    2,
    3)));
extern void __wt_assert( WT_SESSION_IMPL *session,
    const char *check,
    const char *file_name,
    int line_number);
extern int __wt_file_format(WT_SESSION_IMPL *session);
extern int __wt_file_item_too_big(WT_SESSION_IMPL *session);
extern int
__wt_hazard_set(WT_SESSION_IMPL *session, WT_REF *ref
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern void __wt_hazard_clear(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_hazard_empty(WT_SESSION_IMPL *session, const char *name);
extern void __wt_hazard_validate(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_huffman_open(WT_SESSION_IMPL *session,
    void *symbol_frequency_array,
    u_int symcnt,
    u_int numbytes,
    void *retp);
extern void __wt_huffman_close(WT_SESSION_IMPL *session, void *huffman_arg);
extern int __wt_print_huffman_code(void *huffman_arg, uint16_t symbol);
extern int __wt_huffman_encode(WT_SESSION_IMPL *session,
    void *huffman_arg,
    const uint8_t *from_arg,
    uint32_t from_len,
    WT_BUF *to_buf);
extern int __wt_huffman_decode(WT_SESSION_IMPL *session,
    void *huffman_arg,
    const uint8_t *from_arg,
    uint32_t from_len,
    WT_BUF *to_buf);
extern uint32_t __wt_nlpo2_round(uint32_t v);
extern uint32_t __wt_nlpo2(uint32_t v);
extern int __wt_ispo2(uint32_t v);
extern int __wt_buf_init(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size);
extern int __wt_buf_initsize(WT_SESSION_IMPL *session,
    WT_BUF *buf,
    size_t size);
extern int __wt_buf_grow(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size);
extern int __wt_buf_set( WT_SESSION_IMPL *session,
    WT_BUF *buf,
    const void *data,
    uint32_t size);
extern void __wt_buf_steal( WT_SESSION_IMPL *session,
    WT_BUF *buf,
    const void *datap,
    uint32_t *sizep);
extern void __wt_buf_free(WT_SESSION_IMPL *session, WT_BUF *buf);
extern int __wt_scr_alloc(WT_SESSION_IMPL *session,
    uint32_t size,
    WT_BUF **scratchp);
extern void __wt_scr_release(WT_BUF **bufp);
extern void __wt_scr_free(WT_SESSION_IMPL *session);
extern int __wt_session_serialize_func(WT_SESSION_IMPL *session,
    wq_state_t op,
    int spin,
    int (*func)(WT_SESSION_IMPL *),
    void *args);
extern void __wt_session_serialize_wrapup(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    int ret);
extern int __wt_sb_alloc( WT_SESSION_IMPL *session,
    size_t size,
    void *retp,
    WT_SESSION_BUFFER **sbp);
extern void __wt_sb_free(WT_SESSION_IMPL *session, WT_SESSION_BUFFER *sb);
extern void __wt_sb_decrement(WT_SESSION_IMPL *session, WT_SESSION_BUFFER *sb);
extern int __wt_stat_alloc_btree_stats(WT_SESSION_IMPL *session,
    WT_BTREE_STATS **statsp);
extern void __wt_stat_clear_btree_stats(WT_BTREE_STATS *stats);
extern void __wt_stat_print_btree_stats(WT_BTREE_STATS *stats, FILE *stream);
extern int __wt_stat_alloc_btree_file_stats(WT_SESSION_IMPL *session,
    WT_BTREE_FILE_STATS **statsp);
extern void __wt_stat_clear_btree_file_stats(WT_BTREE_FILE_STATS *stats);
extern void __wt_stat_print_btree_file_stats(WT_BTREE_FILE_STATS *stats,
    FILE *stream);
extern int __wt_stat_alloc_cache_stats(WT_SESSION_IMPL *session,
    WT_CACHE_STATS **statsp);
extern void __wt_stat_clear_cache_stats(WT_CACHE_STATS *stats);
extern void __wt_stat_print_cache_stats(WT_CACHE_STATS *stats, FILE *stream);
extern int __wt_stat_alloc_conn_stats(WT_SESSION_IMPL *session,
    WT_CONN_STATS **statsp);
extern void __wt_stat_clear_conn_stats(WT_CONN_STATS *stats);
extern void __wt_stat_print_conn_stats(WT_CONN_STATS *stats, FILE *stream);
extern int __wt_stat_alloc_file_stats(WT_SESSION_IMPL *session,
    WT_FILE_STATS **statsp);
extern void __wt_stat_clear_file_stats(WT_FILE_STATS *stats);
extern void __wt_stat_print_file_stats(WT_FILE_STATS *stats, FILE *stream);

#ifdef __GNUC__
#undef	WT_GCC_ATTRIBUTE
#define	WT_GCC_ATTRIBUTE(x)
#endif
