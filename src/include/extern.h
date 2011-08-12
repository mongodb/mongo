/* DO NOT EDIT: automatically built by dist/s_prototypes. */

#ifdef __GNUC__
#define	WT_GCC_ATTRIBUTE(x)	__attribute__(x)
#else
#define	WT_GCC_ATTRIBUTE(x)
#endif

extern int __wt_open_session(WT_CONNECTION_IMPL *conn,
    WT_EVENT_HANDLER *event_handler,
    const char *config,
    WT_SESSION_IMPL **sessionp);
extern int __wt_config_initn( WT_SESSION_IMPL *session,
    WT_CONFIG *conf,
    const char *str,
    size_t len);
extern int __wt_config_init(WT_SESSION_IMPL *session,
    WT_CONFIG *conf,
    const char *str);
extern int __wt_config_subinit( WT_SESSION_IMPL *session,
    WT_CONFIG *conf,
    WT_CONFIG_ITEM *item);
extern int __wt_config_next(WT_CONFIG *conf,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_getraw( WT_CONFIG *cparser,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_get(WT_SESSION_IMPL *session,
    const char **cfg,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_gets(WT_SESSION_IMPL *session,
    const char **cfg,
    const char *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_getone(WT_SESSION_IMPL *session,
    const char *cfg,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_getones(WT_SESSION_IMPL *session,
    const char *cfg,
    const char *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_subgetraw(WT_SESSION_IMPL *session,
    WT_CONFIG_ITEM *cfg,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_subgets(WT_SESSION_IMPL *session,
    WT_CONFIG_ITEM *cfg,
    const char *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_check(WT_SESSION_IMPL *session,
    const char *checks,
    const char *config);
extern int __wt_config_collapse(WT_SESSION_IMPL *session,
    const char **cfg,
    const char **config_ret);
extern int __wt_config_concat(WT_SESSION_IMPL *session,
    const char **cfg,
    const char **config_ret);
extern const char *__wt_confdfl_colgroup_meta;
extern const char *__wt_confchk_colgroup_meta;
extern const char *__wt_confdfl_connection_add_collator;
extern const char *__wt_confchk_connection_add_collator;
extern const char *__wt_confdfl_connection_add_compressor;
extern const char *__wt_confchk_connection_add_compressor;
extern const char *__wt_confdfl_connection_add_cursor_type;
extern const char *__wt_confchk_connection_add_cursor_type;
extern const char *__wt_confdfl_connection_add_extractor;
extern const char *__wt_confchk_connection_add_extractor;
extern const char *__wt_confdfl_connection_close;
extern const char *__wt_confchk_connection_close;
extern const char *__wt_confdfl_connection_load_extension;
extern const char *__wt_confchk_connection_load_extension;
extern const char *__wt_confdfl_connection_open_session;
extern const char *__wt_confchk_connection_open_session;
extern const char *__wt_confdfl_cursor_close;
extern const char *__wt_confchk_cursor_close;
extern const char *__wt_confdfl_file_meta;
extern const char *__wt_confchk_file_meta;
extern const char *__wt_confdfl_index_meta;
extern const char *__wt_confchk_index_meta;
extern const char *__wt_confdfl_session_begin_transaction;
extern const char *__wt_confchk_session_begin_transaction;
extern const char *__wt_confdfl_session_checkpoint;
extern const char *__wt_confchk_session_checkpoint;
extern const char *__wt_confdfl_session_close;
extern const char *__wt_confchk_session_close;
extern const char *__wt_confdfl_session_commit_transaction;
extern const char *__wt_confchk_session_commit_transaction;
extern const char *__wt_confdfl_session_create;
extern const char *__wt_confchk_session_create;
extern const char *__wt_confdfl_session_drop;
extern const char *__wt_confchk_session_drop;
extern const char *__wt_confdfl_session_dumpfile;
extern const char *__wt_confchk_session_dumpfile;
extern const char *__wt_confdfl_session_log_printf;
extern const char *__wt_confchk_session_log_printf;
extern const char *__wt_confdfl_session_open_cursor;
extern const char *__wt_confchk_session_open_cursor;
extern const char *__wt_confdfl_session_rename;
extern const char *__wt_confchk_session_rename;
extern const char *__wt_confdfl_session_rollback_transaction;
extern const char *__wt_confchk_session_rollback_transaction;
extern const char *__wt_confdfl_session_salvage;
extern const char *__wt_confchk_session_salvage;
extern const char *__wt_confdfl_session_sync;
extern const char *__wt_confchk_session_sync;
extern const char *__wt_confdfl_session_truncate;
extern const char *__wt_confchk_session_truncate;
extern const char *__wt_confdfl_session_verify;
extern const char *__wt_confchk_session_verify;
extern const char *__wt_confdfl_table_meta;
extern const char *__wt_confchk_table_meta;
extern const char *__wt_confdfl_wiredtiger_open;
extern const char *__wt_confchk_wiredtiger_open;
extern int __wt_session_add_btree( WT_SESSION_IMPL *session,
    WT_BTREE_SESSION **btree_sessionp);
extern int __wt_session_find_btree(WT_SESSION_IMPL *session,
    const char *filename,
    size_t namelen,
    WT_BTREE_SESSION **btree_sessionp);
extern int __wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *name,
    const char *filename,
    const char *tconfig);
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
extern void __wt_block_stat(WT_SESSION_IMPL *session);
extern void __wt_block_dump(WT_SESSION_IMPL *session);
extern int __wt_bulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_bulk_end(WT_CURSOR_BULK *cbulk);
extern int __wt_cache_create(WT_CONNECTION_IMPL *conn);
extern void __wt_cache_stats_update(WT_CONNECTION_IMPL *conn);
extern void __wt_cache_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_cell_copy(WT_SESSION_IMPL *session,
    WT_CELL *cell,
    WT_BUF *retb);
extern int __wt_cell_unpack_copy( WT_SESSION_IMPL *session,
    WT_CELL_UNPACK *unpack,
    WT_BUF *retb);
extern int __wt_btree_lex_compare( WT_BTREE *btree,
    const WT_ITEM *user_item,
    const WT_ITEM *tree_item);
extern int __wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact);
extern int __wt_btcur_insert(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_update(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_remove(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_close(WT_CURSOR_BTREE *cbt, const char *config);
extern int __wt_btcur_first(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_next(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_last(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_prev(WT_CURSOR_BTREE *cbt);
extern int __wt_debug_addr( WT_SESSION_IMPL *session,
    uint32_t addr,
    uint32_t size,
    const char *ofile);
extern int __wt_debug_disk(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    const char *ofile);
extern int __wt_debug_tree_all(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const char *ofile);
extern int __wt_debug_tree(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const char *ofile);
extern int __wt_debug_page(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const char *ofile);
extern int __wt_desc_read(WT_SESSION_IMPL *session);
extern int __wt_desc_write(WT_SESSION_IMPL *session, WT_FH *fh);
extern int __wt_desc_update(WT_SESSION_IMPL *session);
extern void __wt_page_out(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    uint32_t flags);
extern void __wt_workq_evict_server(WT_CONNECTION_IMPL *conn, int force);
extern int __wt_evict_file_serial_func(WT_SESSION_IMPL *session);
extern void *__wt_cache_evict_server(void *arg);
extern void __wt_workq_evict_server_exit(WT_CONNECTION_IMPL *conn);
extern int __wt_btree_create(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_btree_root_init(WT_SESSION_IMPL *session);
extern int __wt_btree_open(WT_SESSION_IMPL *session,
    const char *name,
    const char *filename,
    const char *config,
    uint32_t flags);
extern int __wt_btree_close(WT_SESSION_IMPL *session);
extern int __wt_btree_huffman_open(WT_SESSION_IMPL *session);
extern void __wt_btree_huffman_close(WT_SESSION_IMPL *session);
extern const char *__wt_page_type_string(u_int type);
extern const char *__wt_cell_type_string(uint8_t type);
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
extern int __wt_page_reconcile_int(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_SALVAGE_COOKIE *salvage,
    uint32_t flags);
extern void __wt_rec_destroy(WT_SESSION_IMPL *session);
extern int __wt_return_value(WT_SESSION_IMPL *session, WT_CURSOR *cursor);
extern int __wt_disk_read( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size);
extern int __wt_disk_decompress( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *comp_dsk,
    WT_PAGE_DISK *mem_dsk);
extern int __wt_disk_read_scr( WT_SESSION_IMPL *session,
    WT_BUF *buf,
    uint32_t addr,
    uint32_t size);
extern int __wt_disk_write( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size);
extern int __wt_disk_compress(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *mem_dsk,
    WT_PAGE_DISK *comp_dsk,
    uint32_t *psize);
extern int __wt_salvage(WT_SESSION_IMPL *session, const char *config);
extern int __wt_btree_stat_init(WT_SESSION_IMPL *session);
extern int __wt_btree_sync(WT_SESSION_IMPL *session);
extern int __wt_verify(WT_SESSION_IMPL *session, const char *config);
extern int __wt_dumpfile(WT_SESSION_IMPL *session, const char *config);
extern int __wt_verify_dsk(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t size,
    int quiet);
extern int __wt_verify_dsk_chunk( WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk,
    uint32_t addr,
    uint32_t data_len,
    uint32_t memsize,
    int quiet);
extern int __wt_tree_walk(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    int (*work)(WT_SESSION_IMPL *,
    WT_PAGE *,
    void *),
    void *arg);
extern int __wt_walk_first( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_WALK *walk,
    uint32_t flags);
extern int __wt_walk_last( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_WALK *walk,
    uint32_t flags);
extern void __wt_walk_end(WT_SESSION_IMPL *session,
    WT_WALK *walk,
    int discard_walk);
extern int __wt_walk_next(WT_SESSION_IMPL *session,
    WT_WALK *walk,
    WT_PAGE **pagep);
extern int __wt_walk_prev(WT_SESSION_IMPL *session,
    WT_WALK *walk,
    WT_PAGE **pagep);
extern int __wt_col_modify( WT_SESSION_IMPL *session,
    uint64_t recno,
    WT_ITEM *value,
    int is_write);
extern int __wt_col_extend_serial_func(WT_SESSION_IMPL *session);
extern int __wt_col_search(WT_SESSION_IMPL *session,
    uint64_t recno,
    uint32_t flags);
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
extern int __wt_row_modify( WT_SESSION_IMPL *session,
    WT_ITEM *key,
    WT_ITEM *value,
    int is_write);
extern int __wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    uint32_t skipdepth,
    WT_INSERT **insp,
    uint32_t *ins_sizep);
extern int __wt_insert_serial_func(WT_SESSION_IMPL *session);
extern int __wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value,
    WT_UPDATE **updp,
    uint32_t *upd_sizep);
extern int __wt_update_serial_func(WT_SESSION_IMPL *session);
extern int __wt_row_search(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    uint32_t flags);
extern int __wt_connection_config(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_open(WT_CONNECTION_IMPL *conn,
    const char *home,
    mode_t mode);
extern int __wt_connection_close(WT_CONNECTION_IMPL *conn);
extern void __wt_conn_stat_init(WT_SESSION_IMPL *session);
extern void *__wt_workq_srvr(void *arg);
extern int __wt_curbulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_curconfig_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
extern void __wt_curdump_init(WT_CURSOR *cursor, int printable);
extern int __wt_curfile_create(WT_SESSION_IMPL *session,
    int is_public,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_curfile_open(WT_SESSION_IMPL *session,
    const char *name,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_curindex_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_cursor_close(WT_CURSOR *cursor, const char *config);
extern void __wt_cursor_init(WT_CURSOR *cursor,
    int is_public,
    const char *config);
extern int __wt_curtable_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    WT_CURSOR **cursorp);
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
extern int __wt_strndup(WT_SESSION_IMPL *session,
    const char *str,
    size_t len,
    void *retp);
extern int __wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp);
extern void __wt_free_int(WT_SESSION_IMPL *session, void *p_arg);
extern int __wt_dlopen(WT_SESSION_IMPL *session,
    const char *path,
    WT_DLH **dlhp);
extern int __wt_dlsym( WT_SESSION_IMPL *session,
    WT_DLH *dlh,
    const char *name,
    void *sym_ret);
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
extern int __wt_remove(WT_SESSION_IMPL *session, const char *name);
extern int __wt_read(WT_SESSION_IMPL *session,
    WT_FH *fh,
    off_t offset,
    uint32_t bytes,
    void *buf);
extern int __wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh,
    off_t offset,
    uint32_t bytes,
    const void *buf);
extern void __wt_sleep(long seconds, long micro_seconds);
extern int __wt_thread_create(pthread_t *tidret,
    void *(*func)(void *),
    void *arg);
extern void __wt_thread_join(pthread_t tid);
extern void __wt_yield(void);
extern int __wt_schema_create( WT_SESSION_IMPL *session,
    const char *name,
    const char *config);
extern int __wt_schema_drop(WT_SESSION_IMPL *session,
    const char *name,
    const char *config);
extern int __wt_schema_add_table( WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_schema_find_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern int __wt_schema_get_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern int __wt_schema_remove_table( WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_schema_colgroup_name(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *cgname,
    size_t len,
    char **namebufp);
extern int __wt_schema_open_colgroups(WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern int __wt_schema_open_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern int __wt_struct_check(WT_SESSION_IMPL *session,
    const char *fmt,
    size_t len,
    int *fixedp,
    uint32_t *fixed_lenp);
extern size_t __wt_struct_sizev(WT_SESSION_IMPL *session,
    const char *fmt,
    va_list ap);
extern size_t __wt_struct_size(WT_SESSION_IMPL *session, const char *fmt, ...);
extern int __wt_struct_packv(WT_SESSION_IMPL *session,
    void *buffer,
    size_t size,
    const char *fmt,
    va_list ap);
extern int __wt_struct_pack(WT_SESSION_IMPL *session,
    void *buffer,
    size_t size,
    const char *fmt,
    ...);
extern int __wt_struct_unpackv(WT_SESSION_IMPL *session,
    const void *buffer,
    size_t size,
    const char *fmt,
    va_list ap);
extern int __wt_struct_unpack(WT_SESSION_IMPL *session,
    const void *buffer,
    size_t size,
    const char *fmt,
    ...);
extern int __wt_table_check(WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_struct_plan(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *columns,
    size_t len,
    WT_BUF *plan);
extern int __wt_struct_reformat(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *columns,
    size_t len,
    int value_only,
    WT_BUF *format);
extern int __wt_schema_project_in(WT_SESSION_IMPL *session,
    WT_CURSOR **cp,
    const char *proj_arg,
    va_list ap);
extern int __wt_schema_project_out(WT_SESSION_IMPL *session,
    WT_CURSOR **cp,
    const char *proj_arg,
    va_list ap);
extern int __wt_schema_project_slice(WT_SESSION_IMPL *session,
    WT_CURSOR **cp,
    const char *proj_arg,
    const char *vformat,
    WT_ITEM *value);
extern int __wt_schema_project_merge(WT_SESSION_IMPL *session,
    WT_CURSOR **cp,
    const char *proj_arg,
    const char *vformat,
    WT_BUF *value);
extern int __wt_schema_table_cursor(WT_SESSION_IMPL *session,
    WT_CURSOR **cursorp);
extern int __wt_schema_table_insert( WT_SESSION_IMPL *session,
    const char *key,
    const char *value);
extern int __wt_schema_table_remove(WT_SESSION_IMPL *session, const char *key);
extern int __wt_schema_table_read( WT_SESSION_IMPL *session,
    const char *key,
    const char **valuep);
extern uint32_t __wt_cksum(const void *chunk, size_t len);
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
extern void __wt_msgv(WT_SESSION_IMPL *session, const char *fmt, va_list ap);
extern void __wt_msg(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    2,
    3)));
extern int __wt_failure(WT_SESSION_IMPL *session,
    int error,
    const char *file_name,
    int line_number,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    5,
    6)));
extern int __wt_file_format(WT_SESSION_IMPL *session);
extern int __wt_file_item_too_big(WT_SESSION_IMPL *session);
extern int __wt_library_init(void);
extern int __wt_breakpoint(void);
extern void __wt_attach(WT_SESSION_IMPL *session);
extern int
__wt_hazard_set(WT_SESSION_IMPL *session, WT_REF *ref
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern void __wt_hazard_clear(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_hazard_empty(WT_SESSION_IMPL *session);
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
extern uint32_t __wt_random(void);
extern int __wt_buf_init(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size);
extern int __wt_buf_initsize(WT_SESSION_IMPL *session,
    WT_BUF *buf,
    size_t size);
extern int __wt_buf_grow(WT_SESSION_IMPL *session, WT_BUF *buf, size_t size);
extern int __wt_buf_set( WT_SESSION_IMPL *session,
    WT_BUF *buf,
    const void *data,
    size_t size);
extern void *__wt_buf_steal(WT_SESSION_IMPL *session,
    WT_BUF *buf,
    uint32_t *sizep);
extern void __wt_buf_swap(WT_BUF *a, WT_BUF *b);
extern void __wt_buf_free(WT_SESSION_IMPL *session, WT_BUF *buf);
extern int __wt_buf_sprintf(WT_SESSION_IMPL *session,
    WT_BUF *buf,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE ((format (printf,
    3,
    4)));
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
extern void __wt_session_dump_all(WT_SESSION_IMPL *session);
extern void __wt_session_dump(WT_SESSION_IMPL *session);
extern int __wt_stat_alloc_btree_stats(WT_SESSION_IMPL *session,
    WT_BTREE_STATS **statsp);
extern void __wt_stat_clear_btree_stats(WT_STATS *stats_arg);
extern int __wt_stat_alloc_conn_stats(WT_SESSION_IMPL *session,
    WT_CONN_STATS **statsp);
extern void __wt_stat_clear_conn_stats(WT_STATS *stats_arg);

#ifdef __GNUC__
#undef	WT_GCC_ATTRIBUTE
#define	WT_GCC_ATTRIBUTE(x)
#endif
