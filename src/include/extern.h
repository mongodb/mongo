/* DO NOT EDIT: automatically built by dist/s_prototypes. */

extern int __wt_block_addr_to_buffer(WT_BLOCK *block,
    uint8_t **pp,
    off_t offset,
    uint32_t size,
    uint32_t cksum);
extern int __wt_block_buffer_to_addr(WT_BLOCK *block,
    const uint8_t *p,
    off_t *offsetp,
    uint32_t *sizep,
    uint32_t *cksump);
extern int __wt_block_addr_valid(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_addr_string(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_buffer_to_snapshot(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *p,
    WT_BLOCK_SNAPSHOT *si);
extern int __wt_block_snapshot_to_buffer(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    uint8_t **pp,
    WT_BLOCK_SNAPSHOT *si);
extern uint32_t __wt_cksum(const void *chunk, size_t len);
extern int __wt_block_off_remove( WT_SESSION_IMPL *session,
    WT_EXTLIST *el,
    off_t off,
    off_t size);
extern int __wt_block_alloc( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    off_t *offp,
    off_t size);
extern int __wt_block_extend( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    off_t *offp,
    off_t size);
extern int __wt_block_free(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_extlist_check( WT_SESSION_IMPL *session,
    WT_EXTLIST *al,
    WT_EXTLIST *bl);
extern int __wt_block_extlist_match( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_BLOCK_SNAPSHOT *si);
extern int __wt_block_extlist_merge(WT_SESSION_IMPL *session,
    WT_EXTLIST *a,
    WT_EXTLIST *b);
extern int __wt_block_insert_ext( WT_SESSION_IMPL *session,
    WT_EXTLIST *el,
    off_t off,
    off_t size);
extern int __wt_block_extlist_read( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el);
extern int __wt_block_extlist_write( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el);
extern int __wt_block_extlist_truncate( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el);
extern void __wt_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el);
extern void __wt_block_extlist_dump( WT_SESSION_IMPL *session,
    const char *tag,
    WT_EXTLIST *el,
    int show_size);
extern int __wt_bm_addr_valid( WT_SESSION_IMPL *session,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_bm_addr_stderr( WT_SESSION_IMPL *session,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_bm_addr_string(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_bm_create(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_bm_open(WT_SESSION_IMPL *session,
    const char *filename,
    const char *config,
    const char *cfg[]);
extern int __wt_bm_close(WT_SESSION_IMPL *session);
extern int __wt_bm_snapshot(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    WT_SNAPSHOT *snap);
extern int __wt_bm_snapshot_load(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size,
    int readonly);
extern int __wt_bm_snapshot_unload(WT_SESSION_IMPL *session);
extern int __wt_bm_truncate(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_bm_free(WT_SESSION_IMPL *session,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_bm_read(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_bm_write_size(WT_SESSION_IMPL *session, uint32_t *sizep);
extern int __wt_bm_write( WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    uint8_t *addr,
    uint32_t *addr_size);
extern int __wt_bm_stat(WT_SESSION_IMPL *session);
extern int __wt_bm_salvage_start(WT_SESSION_IMPL *session);
extern int __wt_bm_salvage_next(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    uint8_t *addr,
    uint32_t *addr_sizep,
    uint64_t *write_genp,
    int *eofp);
extern int __wt_bm_salvage_end(WT_SESSION_IMPL *session);
extern int __wt_bm_verify_start(WT_SESSION_IMPL *session,
    WT_SNAPSHOT *snapbase);
extern int __wt_bm_verify_end(WT_SESSION_IMPL *session);
extern int __wt_bm_verify_addr(WT_SESSION_IMPL *session,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_truncate(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_block_create(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_block_open(WT_SESSION_IMPL *session,
    const char *filename,
    const char *config,
    const char *cfg[],
    void *retp);
extern int __wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_desc_init(WT_SESSION_IMPL *session, WT_FH *fh);
extern void __wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_block_read(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_read_off(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    off_t offset,
    uint32_t size,
    uint32_t cksum);
extern int __wt_block_salvage_start(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_block_salvage_end(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_block_salvage_next( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    uint8_t *addr,
    uint32_t *addr_sizep,
    uint64_t *write_genp,
    int *eofp);
extern int __wt_block_snap_init(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_BLOCK_SNAPSHOT *si,
    int is_live);
extern int __wt_block_snapshot_load(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *dsk,
    const uint8_t *addr,
    uint32_t addr_size,
    int readonly);
extern int __wt_block_snapshot_unload(WT_SESSION_IMPL *session,
    WT_BLOCK *block);
extern int __wt_block_snapshot(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    WT_SNAPSHOT *snapbase);
extern int __wt_block_verify_start( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_SNAPSHOT *snapbase);
extern int __wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_verify_snap_load( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_BLOCK_SNAPSHOT *si);
extern int __wt_verify_snap_unload( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_BLOCK_SNAPSHOT *si);
extern int __wt_block_verify(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size,
    off_t offset,
    uint32_t size);
extern int __wt_block_verify_addr(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size);
extern u_int __wt_block_header(WT_SESSION_IMPL *session);
extern int __wt_block_write_size( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    uint32_t *sizep);
extern int __wt_block_write(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    uint8_t *addr,
    uint32_t *addr_size);
extern int __wt_block_write_off(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    off_t *offsetp,
    uint32_t *sizep,
    uint32_t *cksump,
    int force_extend);
extern int __wt_bulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_bulk_end(WT_CURSOR_BULK *cbulk);
extern int __wt_cache_create(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern void __wt_cache_stats_update(WT_CONNECTION_IMPL *conn);
extern void __wt_cache_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_cell_copy(WT_SESSION_IMPL *session,
    WT_CELL *cell,
    WT_ITEM *retb);
extern int __wt_cell_unpack_copy( WT_SESSION_IMPL *session,
    WT_CELL_UNPACK *unpack,
    WT_ITEM *retb);
extern void __wt_btcur_iterate_setup(WT_CURSOR_BTREE *cbt, int next);
extern int __wt_btcur_next(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_prev(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_reset(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_search(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact);
extern int __wt_btcur_insert(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_remove(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_update(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_close(WT_CURSOR_BTREE *cbt);
extern int __wt_debug_addr( WT_SESSION_IMPL *session,
    uint32_t addr,
    uint32_t size,
    const char *ofile);
extern int __wt_debug_disk( WT_SESSION_IMPL *session,
    WT_PAGE_HEADER *dsk,
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
extern void __wt_page_out(WT_SESSION_IMPL *session,
    WT_PAGE **pagep,
    uint32_t flags);
extern void __wt_evict_clr_page(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_evict_server_wake(WT_SESSION_IMPL *session);
extern void __wt_sync_file_serial_func(WT_SESSION_IMPL *session);
extern int __wt_evict_page_request(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void *__wt_cache_evict_server(void *arg);
extern int __wt_evict_lru_page(WT_SESSION_IMPL *session, int is_app);
extern int __wt_btree_create(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_btree_truncate(WT_SESSION_IMPL *session, const char *filename);
extern int __wt_btree_open(WT_SESSION_IMPL *session,
    const char *cfg[],
    const uint8_t *addr,
    uint32_t addr_size,
    int readonly);
extern int __wt_btree_close(WT_SESSION_IMPL *session);
extern int __wt_btree_tree_open(WT_SESSION_IMPL *session, WT_ITEM *dsk);
extern int __wt_btree_root_empty(WT_SESSION_IMPL *session, WT_PAGE **leafp);
extern int __wt_btree_huffman_open(WT_SESSION_IMPL *session,
    const char *config);
extern void __wt_btree_huffman_close(WT_SESSION_IMPL *session);
extern const char *__wt_page_type_string(u_int type);
extern const char *__wt_cell_type_string(uint8_t type);
extern const char *__wt_page_addr_string(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    WT_PAGE *page);
extern const char *__wt_addr_string( WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t size);
extern int __wt_ovfl_in( WT_SESSION_IMPL *session,
    WT_ITEM *store,
    const uint8_t *addr,
    uint32_t len);
extern int
__wt_page_in_func(
 WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern int __wt_page_inmem(WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *parent_ref,
    WT_PAGE_HEADER *dsk,
    size_t *inmem_sizep,
    WT_PAGE **pagep);
extern int __wt_cache_read(WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *ref);
extern int __wt_kv_return(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int key_ret);
extern int __wt_salvage(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_btree_stat_init(WT_SESSION_IMPL *session);
extern int __wt_btree_snapshot(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_btree_snapshot_close(WT_SESSION_IMPL *session);
extern int __wt_btree_snapshot_drop(WT_SESSION_IMPL *session,
    const char *cfg[]);
extern int __wt_cache_flush(WT_SESSION_IMPL *session, int op);
extern int __wt_upgrade(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_verify(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_dumpfile(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_verify_dsk(WT_SESSION_IMPL *session,
    const char *addr,
    WT_ITEM *buf);
extern int __wt_tree_np(WT_SESSION_IMPL *session,
    WT_PAGE **pagep,
    int eviction,
    int next);
extern int __wt_col_modify(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int op);
extern void __wt_col_append_serial_func(WT_SESSION_IMPL *session);
extern int __wt_col_search(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int is_modify);
extern int __wt_rec_evict(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    uint32_t flags);
extern int __wt_rec_track_block(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const uint8_t *addr,
    uint32_t size,
    int permanent);
extern int __wt_rec_track_ovfl(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const uint8_t *addr,
    uint32_t addr_size,
    const void *data,
    uint32_t data_size,
    uint32_t flags);
extern int __wt_rec_track_ovfl_reuse(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const void *data,
    uint32_t size,
    uint8_t **addrp,
    uint32_t *sizep);
extern int __wt_rec_track_ovfl_srch(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const uint8_t *addr,
    uint32_t size,
    WT_ITEM *copy);
extern int __wt_rec_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_rec_track_discard(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_rec_write( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_SALVAGE_COOKIE *salvage);
extern void __wt_rec_destroy(WT_SESSION_IMPL *session);
extern int __wt_rec_bulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_bulk_wrapup(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_row_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_col_fix_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_col_var_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_row_leaf_keys(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_row_key( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_ROW *rip_arg,
    WT_ITEM *retb);
extern WT_CELL *__wt_row_value(WT_PAGE *page, WT_ROW *rip);
extern int __wt_row_ikey_alloc(WT_SESSION_IMPL *session,
    uint32_t cell_offset,
    const void *key,
    uint32_t size,
    WT_IKEY **ikeyp);
extern void __wt_row_key_serial_func(WT_SESSION_IMPL *session);
extern int __wt_row_modify(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int is_remove);
extern int __wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    u_int skipdepth,
    WT_INSERT **insp,
    size_t *ins_sizep);
extern void __wt_insert_serial_func(WT_SESSION_IMPL *session);
extern int __wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value,
    WT_UPDATE **updp,
    size_t *sizep);
extern void __wt_update_serial_func(WT_SESSION_IMPL *session);
extern WT_INSERT *__wt_search_insert(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    WT_INSERT_HEAD *inshead,
    WT_ITEM *srch_key);
extern int __wt_row_search(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int is_modify);
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
extern int __wt_config_concat( WT_SESSION_IMPL *session,
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
extern const char *__wt_confdfl_session_upgrade;
extern const char *__wt_confchk_session_upgrade;
extern const char *__wt_confdfl_session_verify;
extern const char *__wt_confchk_session_verify;
extern const char *__wt_confdfl_table_meta;
extern const char *__wt_confchk_table_meta;
extern const char *__wt_confdfl_wiredtiger_open;
extern const char *__wt_confchk_wiredtiger_open;
extern int __wt_conn_btree_open(WT_SESSION_IMPL *session,
    const char *name,
    const char *filename,
    const char *config,
    const char *cfg[],
    uint32_t flags);
extern int __wt_conn_btree_close(WT_SESSION_IMPL *session, int locked);
extern int __wt_conn_btree_remove(WT_CONNECTION_IMPL *conn);
extern int __wt_conn_btree_reopen( WT_SESSION_IMPL *session,
    const char *cfg[],
    uint32_t flags);
extern int __wt_connection_init(WT_CONNECTION_IMPL *conn);
extern void __wt_connection_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern int __wt_connection_close(WT_CONNECTION_IMPL *conn);
extern void __wt_conn_stat_init(WT_SESSION_IMPL *session);
extern int __wt_curbulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_curconfig_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curdump_create(WT_CURSOR *child,
    WT_CURSOR *owner,
    WT_CURSOR **cursorp);
extern int __wt_curfile_create(WT_SESSION_IMPL *session,
    WT_CURSOR *owner,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curfile_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curindex_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_cursor_notsup(WT_CURSOR *cursor);
extern int __wt_cursor_get_key(WT_CURSOR *cursor, ...);
extern int __wt_cursor_get_keyv(WT_CURSOR *cursor, uint32_t flags, va_list ap);
extern int __wt_cursor_get_value(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_key(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_keyv(WT_CURSOR *cursor, uint32_t flags, va_list ap);
extern void __wt_cursor_set_value(WT_CURSOR *cursor, ...);
extern int __wt_cursor_close(WT_CURSOR *cursor);
extern int __wt_cursor_dup(WT_SESSION_IMPL *session,
    WT_CURSOR *to_dup,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_cursor_init(WT_CURSOR *cursor,
    const char *uri,
    WT_CURSOR *owner,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_cursor_kv_not_set(WT_CURSOR *cursor, int key);
extern int __wt_curtable_get_key(WT_CURSOR *cursor, ...);
extern int __wt_curtable_get_value(WT_CURSOR *cursor, ...);
extern void __wt_curtable_set_key(WT_CURSOR *cursor, ...);
extern void __wt_curtable_set_value(WT_CURSOR *cursor, ...);
extern int __wt_curtable_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_log_put(WT_SESSION_IMPL *session, WT_LOGREC_DESC *recdesc, ...);
extern int __wt_log_vprintf(WT_SESSION_IMPL *session,
    const char *fmt,
    va_list ap);
extern int __wt_log_printf(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    2,
    3)));
extern WT_LOGREC_DESC __wt_logdesc_debug;
extern void __wt_abort(WT_SESSION_IMPL *session);
extern int __wt_calloc(WT_SESSION_IMPL *session,
    size_t number,
    size_t size,
    void *retp);
extern int __wt_realloc(WT_SESSION_IMPL *session,
    size_t *bytes_allocated_ret,
    size_t bytes_to_allocate,
    void *retp);
extern int __wt_realloc_aligned(WT_SESSION_IMPL *session,
    size_t *bytes_allocated_ret,
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
extern int __wt_errno(void);
extern int __wt_exist(WT_SESSION_IMPL *session,
    const char *filename,
    int *existp);
extern int __wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, off_t *sizep);
extern int __wt_bytelock(WT_FH *fhp, off_t byte, int lock);
extern int __wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh);
extern int __wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, off_t len);
extern int __wt_cond_alloc(WT_SESSION_IMPL *session,
    const char *name,
    int is_locked,
    WT_CONDVAR **condp);
extern void __wt_cond_wait(WT_SESSION_IMPL *session, WT_CONDVAR *cond);
extern void __wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond);
extern int __wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR *cond);
extern int __wt_rwlock_alloc( WT_SESSION_IMPL *session,
    const char *name,
    WT_RWLOCK **rwlockp);
extern void __wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern void __wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern void __wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_open(WT_SESSION_IMPL *session,
    const char *name,
    int ok_create,
    int exclusive,
    int is_tree,
    WT_FH **fhp);
extern int __wt_close(WT_SESSION_IMPL *session, WT_FH *fh);
extern int __wt_has_priv(void);
extern int __wt_remove(WT_SESSION_IMPL *session, const char *name);
extern int __wt_rename(WT_SESSION_IMPL *session,
    const char *from,
    const char *to);
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
extern int __wt_thread_join(pthread_t tid);
extern int __wt_epoch(WT_SESSION_IMPL *session,
    uint64_t *secp,
    uint64_t *nsecp);
extern void __wt_yield(void);
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
extern int __wt_create_file(WT_SESSION_IMPL *session,
    const char *name,
    const char *fileuri,
    int exclusive,
    const char *config);
extern int __wt_schema_create( WT_SESSION_IMPL *session,
    const char *name,
    const char *config);
extern int __wt_schema_drop(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[]);
extern int __wt_schema_add_table( WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_schema_find_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern int __wt_schema_get_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern void __wt_schema_destroy_table(WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern int __wt_schema_remove_table( WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_schema_close_tables(WT_SESSION_IMPL *session);
extern void __wt_schema_detach_tree(WT_SESSION_IMPL *session, WT_BTREE *btree);
extern int __wt_schema_colgroup_name(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *cgname,
    size_t len,
    char **namebufp);
extern int __wt_schema_get_btree(WT_SESSION_IMPL *session,
    const char *objname,
    size_t len,
    const char *cfg[],
    uint32_t flags);
extern int __wt_schema_open_colgroups(WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern int __wt_schema_open_index( WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *idxname,
    size_t len);
extern int __wt_schema_open_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern int __wt_schema_colcheck(WT_SESSION_IMPL *session,
    const char *key_format,
    const char *value_format,
    WT_CONFIG_ITEM *colconf,
    int *kcolsp,
    int *vcolsp);
extern int __wt_table_check(WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_struct_plan(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *columns,
    size_t len,
    int value_only,
    WT_ITEM *plan);
extern int __wt_struct_reformat(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *columns,
    size_t len,
    const char *extra_cols,
    int value_only,
    WT_ITEM *format);
extern int __wt_struct_truncate(WT_SESSION_IMPL *session,
    const char *input_fmt,
    u_int ncols,
    WT_ITEM *format);
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
    int key_only,
    const char *vformat,
    WT_ITEM *value);
extern int __wt_schema_project_merge(WT_SESSION_IMPL *session,
    WT_CURSOR **cp,
    const char *proj_arg,
    const char *vformat,
    WT_ITEM *value);
extern int __wt_schema_rename(WT_SESSION_IMPL *session,
    const char *uri,
    const char *newuri,
    const char *cfg[]);
extern int __wt_open_schema_table(WT_SESSION_IMPL *session);
extern int __wt_schema_table_cursor( WT_SESSION_IMPL *session,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_schema_table_insert( WT_SESSION_IMPL *session,
    const char *key,
    const char *value);
extern int __wt_schema_table_update( WT_SESSION_IMPL *session,
    const char *key,
    const char *value);
extern int __wt_schema_table_remove(WT_SESSION_IMPL *session, const char *key);
extern int __wt_schema_table_read( WT_SESSION_IMPL *session,
    const char *key,
    const char **valuep);
extern int __wt_schema_table_track_on(WT_SESSION_IMPL *session);
extern int __wt_schema_table_track_off(WT_SESSION_IMPL *session, int unroll);
extern int __wt_schema_table_track_insert(WT_SESSION_IMPL *session,
    const char *key);
extern int __wt_schema_table_track_update(WT_SESSION_IMPL *session,
    const char *key);
extern int __wt_schema_table_track_fileop( WT_SESSION_IMPL *session,
    const char *oldname,
    const char *newname);
extern int __wt_schema_truncate( WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[]);
extern int __wt_schema_name_check(WT_SESSION_IMPL *session, const char *uri);
extern int __wt_schema_worker(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    int (*func)(WT_SESSION_IMPL *,
    const char *[]),
    uint32_t open_flags);
extern int __wt_session_create_strip( WT_SESSION *session,
    const char *v1,
    const char *v2,
    const char **value_ret);
extern int __wt_open_session(WT_CONNECTION_IMPL *conn,
    int internal,
    WT_EVENT_HANDLER *event_handler,
    const char *config,
    WT_SESSION_IMPL **sessionp);
extern int __wt_session_add_btree( WT_SESSION_IMPL *session,
    WT_BTREE_SESSION **btree_sessionp);
extern int __wt_session_lock_btree( WT_SESSION_IMPL *session,
    const char *cfg[],
    uint32_t flags);
extern int __wt_session_release_btree(WT_SESSION_IMPL *session);
extern int __wt_session_find_btree(WT_SESSION_IMPL *session,
    const char *filename,
    size_t namelen,
    const char *cfg[],
    uint32_t flags,
    WT_BTREE_SESSION **btree_sessionp);
extern int __wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *name,
    const char *fileuri,
    const char *tconfig,
    const char *cfg[],
    uint32_t flags);
extern int __wt_session_remove_btree( WT_SESSION_IMPL *session,
    WT_BTREE_SESSION *btree_session,
    int locked);
extern int __wt_session_close_any_open_btree(WT_SESSION_IMPL *session,
    const char *name);
extern int __wt_session_snap_get(WT_SESSION_IMPL *session,
    const char *name,
    WT_ITEM *addr);
extern int __wt_session_snap_clear(WT_SESSION_IMPL *session,
    const char *filename);
extern int __wt_snap_list_get( WT_SESSION *session,
    const char *config,
    WT_SNAPSHOT **snapbasep);
extern int __wt_session_snap_list_get( WT_SESSION_IMPL *session,
    const char *config_arg,
    WT_SNAPSHOT **snapbasep);
extern int __wt_session_snap_list_set(WT_SESSION_IMPL *session,
    WT_SNAPSHOT *snapbase);
extern void __wt_snap_list_free(WT_SESSION *session, WT_SNAPSHOT *snapbase);
extern void __wt_session_snap_list_free(WT_SESSION_IMPL *session,
    WT_SNAPSHOT *snapbase);
extern void __wt_eventv(WT_SESSION_IMPL *session,
    int msg_event,
    int error,
    const char *file_name,
    int line_number,
    const char *fmt,
    va_list ap);
extern void __wt_err(WT_SESSION_IMPL *session,
    int error,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    3,
    4)));
extern void __wt_errx(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    2,
    3)));
extern void __wt_msgv(WT_SESSION_IMPL *session, const char *fmt, va_list ap);
extern void __wt_verbose(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    2,
    3)));
extern void __wt_msg(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    2,
    3)));
extern int __wt_assert(WT_SESSION_IMPL *session,
    int error,
    const char *file_name,
    int line_number,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    5,
    6)));
extern int __wt_illegal_value(WT_SESSION_IMPL *session);
extern int __wt_unknown_object_type(WT_SESSION_IMPL *session, const char *uri);
extern int __wt_filename(WT_SESSION_IMPL *session,
    const char *name,
    const char **path);
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
extern int __wt_raw_to_hex( WT_SESSION_IMPL *session,
    const uint8_t *from,
    uint32_t size,
    WT_ITEM *to);
extern int __wt_raw_to_esc_hex( WT_SESSION_IMPL *session,
    const uint8_t *from,
    size_t size,
    WT_ITEM *to);
extern int __wt_hex_to_raw(WT_SESSION_IMPL *session,
    const char *from,
    WT_ITEM *to);
extern int __wt_nhex_to_raw( WT_SESSION_IMPL *session,
    const char *from,
    size_t size,
    WT_ITEM *to);
extern int __wt_esc_hex_to_raw(WT_SESSION_IMPL *session,
    const char *from,
    WT_ITEM *to);
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
    WT_ITEM *to_buf);
extern int __wt_huffman_decode(WT_SESSION_IMPL *session,
    void *huffman_arg,
    const uint8_t *from_arg,
    uint32_t from_len,
    WT_ITEM *to_buf);
extern uint32_t __wt_nlpo2_round(uint32_t v);
extern uint32_t __wt_nlpo2(uint32_t v);
extern int __wt_ispo2(uint32_t v);
extern uint32_t __wt_random(void);
extern int __wt_buf_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size);
extern int __wt_buf_init(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size);
extern int __wt_buf_initsize(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    size_t size);
extern int __wt_buf_set( WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const void *data,
    size_t size);
extern int __wt_buf_set_printable( WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const void *from_arg,
    size_t size);
extern void *__wt_buf_steal(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    uint32_t *sizep);
extern void __wt_buf_free(WT_SESSION_IMPL *session, WT_ITEM *buf);
extern int __wt_buf_fmt(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    3,
    4)));
extern int __wt_buf_catfmt(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    3,
    4)));
extern int __wt_scr_alloc(WT_SESSION_IMPL *session,
    uint32_t size,
    WT_ITEM **scratchp);
extern void __wt_scr_free(WT_ITEM **bufp);
extern void __wt_scr_discard(WT_SESSION_IMPL *session);
extern void *__wt_scr_alloc_ext(WT_SESSION *wt_session, size_t size);
extern void __wt_scr_free_ext(WT_SESSION *wt_session, void *p);
extern void __wt_session_dump_all(WT_SESSION_IMPL *session);
extern void __wt_session_dump(WT_SESSION_IMPL *session);
extern int __wt_stat_alloc_btree_stats(WT_SESSION_IMPL *session,
    WT_BTREE_STATS **statsp);
extern void __wt_stat_clear_btree_stats(WT_STATS *stats_arg);
extern int __wt_stat_alloc_connection_stats(WT_SESSION_IMPL *session,
    WT_CONNECTION_STATS **statsp);
extern void __wt_stat_clear_connection_stats(WT_STATS *stats_arg);
