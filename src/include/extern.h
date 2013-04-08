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
extern int __wt_block_buffer_to_ckpt(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *p,
    WT_BLOCK_CKPT *ci);
extern int __wt_block_ckpt_to_buffer(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    uint8_t **pp,
    WT_BLOCK_CKPT *ci);
extern int __wt_block_ckpt_init( WT_SESSION_IMPL *session,
    WT_BLOCK_CKPT *ci,
    const char *name);
extern int __wt_block_checkpoint_load(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size,
    uint8_t *root_addr,
    uint32_t *root_addr_size,
    int checkpoint);
extern int __wt_block_checkpoint_unload( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    int checkpoint);
extern void __wt_block_ckpt_destroy(WT_SESSION_IMPL *session,
    WT_BLOCK_CKPT *ci);
extern int __wt_block_checkpoint(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    WT_CKPT *ckptbase,
    int data_cksum);
extern int __wt_block_checkpoint_resolve(WT_SESSION_IMPL *session,
    WT_BLOCK *block);
extern int __wt_block_compact_skip( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    int trigger,
    int *skipp);
extern int __wt_block_compact_page_skip(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size,
    int *skipp);
extern void __wt_block_ext_cleanup(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_block_misplaced(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const char *tag,
    off_t offset,
    uint32_t size);
extern int __wt_block_off_remove_overlap(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el,
    off_t off,
    off_t size);
extern int __wt_block_alloc( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    off_t *offp,
    off_t size);
extern int __wt_block_free(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_off_free( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    off_t offset,
    off_t size);
extern int __wt_block_extlist_check( WT_SESSION_IMPL *session,
    WT_EXTLIST *al,
    WT_EXTLIST *bl);
extern int __wt_block_extlist_overlap( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_BLOCK_CKPT *ci);
extern int __wt_block_extlist_merge( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *a,
    WT_EXTLIST *b);
extern int __wt_block_insert_ext(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el,
    off_t off,
    off_t size);
extern int __wt_block_extlist_read_avail( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el,
    off_t ckpt_size);
extern int __wt_block_extlist_read( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el,
    off_t ckpt_size);
extern int __wt_block_extlist_write(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el,
    WT_EXTLIST *additional);
extern int __wt_block_extlist_truncate( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_EXTLIST *el);
extern int __wt_block_extlist_init(WT_SESSION_IMPL *session,
    WT_EXTLIST *el,
    const char *name,
    const char *extname);
extern void __wt_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el);
extern int __wt_block_map( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    void *mapp,
    size_t *maplenp);
extern int __wt_block_unmap( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    void *map,
    size_t maplen);
extern int __wt_block_manager_open(WT_SESSION_IMPL *session,
    const char *filename,
    const char *cfg[],
    int forced_salvage,
    WT_BM **bmp);
extern int __wt_block_manager_truncate(WT_SESSION_IMPL *session,
    const char *filename);
extern int __wt_block_manager_create(WT_SESSION_IMPL *session,
    const char *filename);
extern int __wt_block_open(WT_SESSION_IMPL *session,
    const char *filename,
    const char *cfg[],
    int forced_salvage,
    WT_BLOCK **blockp);
extern int __wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_desc_init(WT_SESSION_IMPL *session, WT_FH *fh);
extern void __wt_block_stat(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_DSRC_STATS *stats);
extern int __wt_bm_read(WT_BM *bm,
    WT_SESSION_IMPL *session,
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
extern int __wt_block_salvage_next(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    uint8_t *addr,
    uint32_t *addr_sizep,
    int *eofp);
extern int __wt_block_salvage_valid(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    uint8_t *addr,
    uint32_t addr_size);
extern int __wt_block_verify_start( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_CKPT *ckptbase);
extern int __wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_verify_ckpt_load( WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_BLOCK_CKPT *ci);
extern int __wt_verify_ckpt_unload(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern int __wt_block_verify_addr(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    const uint8_t *addr,
    uint32_t addr_size);
extern u_int __wt_block_header(WT_BLOCK *block);
extern int __wt_block_write_size(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    size_t *sizep);
extern int __wt_block_write(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    uint8_t *addr,
    uint32_t *addr_size,
    int data_cksum);
extern int __wt_block_write_off(WT_SESSION_IMPL *session,
    WT_BLOCK *block,
    WT_ITEM *buf,
    off_t *offsetp,
    uint32_t *sizep,
    uint32_t *cksump,
    int data_cksum,
    int locked);
extern int __wt_bloom_create( WT_SESSION_IMPL *session,
    const char *uri,
    const char *config,
    uint64_t count,
    uint32_t factor,
    uint32_t k,
    WT_BLOOM **bloomp);
extern int __wt_bloom_open(WT_SESSION_IMPL *session,
    const char *uri,
    uint32_t factor,
    uint32_t k,
    WT_CURSOR *owner,
    WT_BLOOM **bloomp);
extern int __wt_bloom_insert(WT_BLOOM *bloom, WT_ITEM *key);
extern int __wt_bloom_finalize(WT_BLOOM *bloom);
extern int __wt_bloom_hash(WT_BLOOM *bloom, WT_ITEM *key, WT_BLOOM_HASH *bhash);
extern int __wt_bloom_hash_get(WT_BLOOM *bloom, WT_BLOOM_HASH *bhash);
extern int __wt_bloom_get(WT_BLOOM *bloom, WT_ITEM *key);
extern int __wt_bloom_close(WT_BLOOM *bloom);
extern int __wt_bloom_drop(WT_BLOOM *bloom, const char *config);
extern int __wt_bulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_bulk_end(WT_CURSOR_BULK *cbulk);
extern int __wt_compact(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_compact_page_skip( WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *ref,
    int *skipp);
extern int __wt_compact_evict(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_btcur_iterate_setup(WT_CURSOR_BTREE *cbt, int next);
extern int __wt_btcur_next(WT_CURSOR_BTREE *cbt, int discard);
extern int __wt_btcur_next_random(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_prev(WT_CURSOR_BTREE *cbt, int discard);
extern int __wt_btcur_reset(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_search(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact);
extern int __wt_btcur_insert(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_remove(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_update(WT_CURSOR_BTREE *cbt);
extern int __wt_btcur_compare(WT_CURSOR_BTREE *a_arg,
    WT_CURSOR_BTREE *b_arg,
    int *cmpp);
extern int __wt_btcur_truncate(WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop);
extern int __wt_btcur_close(WT_CURSOR_BTREE *cbt);
extern int __wt_debug_addr(WT_SESSION_IMPL *session,
    const uint8_t *addr,
    uint32_t addr_size,
    const char *ofile);
extern int __wt_debug_offset(WT_SESSION_IMPL *session,
    off_t offset,
    uint32_t size,
    uint32_t cksum,
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
extern void __wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep);
extern void __wt_evict_list_clr_page(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_evict_forced_page(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_evict_server_wake(WT_SESSION_IMPL *session);
extern int __wt_sync_file_serial_func(WT_SESSION_IMPL *session, void *args);
extern void *__wt_cache_evict_server(void *arg);
extern void __wt_evict_clear_tree_walk(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_evict_page(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_evict_file(WT_SESSION_IMPL *session, int syncop);
extern int __wt_sync_file(WT_SESSION_IMPL *session, int syncop);
extern int __wt_evict_lru_page(WT_SESSION_IMPL *session, int is_app);
extern int __wt_btree_open(WT_SESSION_IMPL *session, const char *op_cfg[]);
extern int __wt_btree_close(WT_SESSION_IMPL *session);
extern int __wt_btree_tree_open( WT_SESSION_IMPL *session,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_btree_leaf_create( WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *ref,
    WT_PAGE **pagep);
extern void __wt_btree_evictable(WT_SESSION_IMPL *session, int on);
extern uint32_t __wt_split_page_size(WT_BTREE *btree, uint32_t maxpagesize);
extern int __wt_btree_huffman_open(WT_SESSION_IMPL *session);
extern void __wt_btree_huffman_close(WT_SESSION_IMPL *session);
extern int __wt_bt_read(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_bt_write(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    uint8_t *addr,
    uint32_t *addr_size,
    int checkpoint,
    int compressed);
extern const char *__wt_page_type_string(u_int type);
extern const char *__wt_cell_type_string(uint8_t type);
extern const char *__wt_page_addr_string(WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    WT_PAGE *page);
extern const char *__wt_addr_string( WT_SESSION_IMPL *session,
    WT_ITEM *buf,
    const uint8_t *addr,
    uint32_t size);
extern int __wt_ovfl_read(WT_SESSION_IMPL *session,
    WT_CELL_UNPACK *unpack,
    WT_ITEM *store);
extern int __wt_ovfl_cache_col_restart(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_CELL_UNPACK *unpack,
    WT_ITEM *store);
extern int __wt_val_ovfl_cache(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    void *cookie,
    WT_CELL_UNPACK *unpack);
extern int
__wt_page_in_func(
 WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern int __wt_page_alloc(WT_SESSION_IMPL *session,
    uint8_t type,
    uint32_t alloc_entries,
    WT_PAGE **pagep);
extern int __wt_page_inmem( WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *parent_ref,
    WT_PAGE_HEADER *dsk,
    int disk_not_alloc,
    WT_PAGE **pagep);
extern int __wt_cache_read(WT_SESSION_IMPL *session,
    WT_PAGE *parent,
    WT_REF *ref);
extern int __wt_kv_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt);
extern int __wt_bt_salvage(WT_SESSION_IMPL *session,
    WT_CKPT *ckptbase,
    const char *cfg[]);
extern int __wt_btree_stat_init(WT_SESSION_IMPL *session, uint32_t flags);
extern int __wt_bt_cache_force_write(WT_SESSION_IMPL *session);
extern int __wt_bt_cache_op(WT_SESSION_IMPL *session,
    WT_CKPT *ckptbase,
    int op);
extern int __wt_upgrade(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_verify(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_verify_dsk(WT_SESSION_IMPL *session,
    const char *addr,
    WT_ITEM *buf);
extern void __wt_tree_walk_delete_rollback(WT_REF *ref);
extern int __wt_tree_walk(WT_SESSION_IMPL *session,
    WT_PAGE **pagep,
    uint32_t flags);
extern int __wt_col_modify(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int op);
extern int __wt_col_append_serial_func(WT_SESSION_IMPL *session, void *args);
extern void __wt_col_leaf_obsolete(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_col_search(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int is_modify);
extern int __wt_rec_evict(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    int exclusive);
extern int __wt_merge_tree(WT_SESSION_IMPL *session, WT_PAGE *top);
extern int __wt_rec_track(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const uint8_t *addr,
    uint32_t addr_size,
    const void *data,
    uint32_t data_size,
    uint32_t flags);
extern int __wt_rec_track_ovfl_srch( WT_PAGE *page,
    const uint8_t *addr,
    uint32_t addr_size,
    WT_ITEM *data);
extern int __wt_rec_track_onpage_srch( WT_PAGE *page,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_rec_track_onpage_addr(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const uint8_t *addr,
    uint32_t addr_size);
extern int __wt_rec_track_ovfl_reuse( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    const void *data,
    uint32_t data_size,
    uint8_t **addrp,
    uint32_t *addr_sizep,
    int *foundp);
extern int __wt_rec_track_init(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_rec_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_rec_track_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_rec_track_discard(WT_SESSION_IMPL *session, WT_PAGE *page);
extern char *__wt_track_string(WT_PAGE_TRACK *track, char *buf, size_t len);
extern int __wt_rec_write(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_SALVAGE_COOKIE *salvage,
    uint32_t flags);
extern void __wt_rec_destroy(WT_SESSION_IMPL *session, void *retp);
extern int __wt_rec_bulk_init(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_bulk_wrapup(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_row_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_col_fix_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_rec_col_var_bulk_insert(WT_CURSOR_BULK *cbulk);
extern int __wt_row_leaf_keys(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_row_key_copy( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_ROW *rip_arg,
    WT_ITEM *retb);
extern WT_CELL *__wt_row_value(WT_PAGE *page, WT_ROW *rip);
extern int __wt_row_ikey_incr(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    uint32_t cell_offset,
    const void *key,
    uint32_t size,
    void *ikeyp);
extern int __wt_row_ikey(WT_SESSION_IMPL *session,
    uint32_t cell_offset,
    const void *key,
    uint32_t size,
    void *ikeyp);
extern int __wt_row_modify(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int is_remove);
extern int __wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key,
    u_int skipdepth,
    WT_INSERT **insp,
    size_t *ins_sizep);
extern int __wt_insert_serial_func(WT_SESSION_IMPL *session, void *args);
extern int __wt_update_check(WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_UPDATE *next);
extern int __wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value,
    WT_UPDATE **updp,
    size_t *sizep);
extern WT_UPDATE *__wt_update_obsolete_check(WT_SESSION_IMPL *session,
    WT_UPDATE *upd);
extern void __wt_update_obsolete_free( WT_SESSION_IMPL *session,
    WT_PAGE *page,
    WT_UPDATE *upd);
extern void __wt_row_leaf_obsolete(WT_SESSION_IMPL *session, WT_PAGE *page);
extern int __wt_update_serial_func(WT_SESSION_IMPL *session, void *args);
extern int __wt_search_insert(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    WT_INSERT_HEAD *inshead,
    WT_ITEM *srch_key);
extern int __wt_row_search(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt,
    int is_modify);
extern int __wt_row_random(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt);
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
extern int __wt_config_get(WT_SESSION_IMPL *session,
    const char **cfg,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_gets(WT_SESSION_IMPL *session,
    const char **cfg,
    const char *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_getone(WT_SESSION_IMPL *session,
    const char *config,
    WT_CONFIG_ITEM *key,
    WT_CONFIG_ITEM *value);
extern  int __wt_config_getones(WT_SESSION_IMPL *session,
    const char *config,
    const char *key,
    WT_CONFIG_ITEM *value);
extern int __wt_config_gets_defno(WT_SESSION_IMPL *session,
    const char **cfg,
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
    WT_CONFIG_CHECK checks[],
    const char *config,
    size_t config_len);
extern int __wt_config_collapse( WT_SESSION_IMPL *session,
    const char **cfg,
    const char **config_ret);
extern int __wt_config_concat( WT_SESSION_IMPL *session,
    const char **cfg,
    const char **config_ret);
extern const char *__wt_confdfl_colgroup_meta;
extern WT_CONFIG_CHECK __wt_confchk_colgroup_meta[];
extern const char *__wt_confdfl_connection_add_collator;
extern WT_CONFIG_CHECK __wt_confchk_connection_add_collator[];
extern const char *__wt_confdfl_connection_add_compressor;
extern WT_CONFIG_CHECK __wt_confchk_connection_add_compressor[];
extern const char *__wt_confdfl_connection_add_data_source;
extern WT_CONFIG_CHECK __wt_confchk_connection_add_data_source[];
extern const char *__wt_confdfl_connection_add_extractor;
extern WT_CONFIG_CHECK __wt_confchk_connection_add_extractor[];
extern const char *__wt_confdfl_connection_close;
extern WT_CONFIG_CHECK __wt_confchk_connection_close[];
extern const char *__wt_confdfl_connection_load_extension;
extern WT_CONFIG_CHECK __wt_confchk_connection_load_extension[];
extern const char *__wt_confdfl_connection_open_session;
extern WT_CONFIG_CHECK __wt_confchk_connection_open_session[];
extern const char *__wt_confdfl_connection_reconfigure;
extern WT_CONFIG_CHECK __wt_confchk_shared_cache_subconfigs[];
extern WT_CONFIG_CHECK __wt_confchk_connection_reconfigure[];
extern const char *__wt_confdfl_cursor_close;
extern WT_CONFIG_CHECK __wt_confchk_cursor_close[];
extern const char *__wt_confdfl_file_meta;
extern WT_CONFIG_CHECK __wt_confchk_file_meta[];
extern const char *__wt_confdfl_index_meta;
extern WT_CONFIG_CHECK __wt_confchk_index_meta[];
extern const char *__wt_confdfl_session_begin_transaction;
extern WT_CONFIG_CHECK __wt_confchk_session_begin_transaction[];
extern const char *__wt_confdfl_session_checkpoint;
extern WT_CONFIG_CHECK __wt_confchk_session_checkpoint[];
extern const char *__wt_confdfl_session_close;
extern WT_CONFIG_CHECK __wt_confchk_session_close[];
extern const char *__wt_confdfl_session_commit_transaction;
extern WT_CONFIG_CHECK __wt_confchk_session_commit_transaction[];
extern const char *__wt_confdfl_session_compact;
extern WT_CONFIG_CHECK __wt_confchk_session_compact[];
extern const char *__wt_confdfl_session_create;
extern WT_CONFIG_CHECK __wt_confchk_session_create[];
extern const char *__wt_confdfl_session_drop;
extern WT_CONFIG_CHECK __wt_confchk_session_drop[];
extern const char *__wt_confdfl_session_log_printf;
extern WT_CONFIG_CHECK __wt_confchk_session_log_printf[];
extern const char *__wt_confdfl_session_open_cursor;
extern WT_CONFIG_CHECK __wt_confchk_session_open_cursor[];
extern const char *__wt_confdfl_session_reconfigure;
extern WT_CONFIG_CHECK __wt_confchk_session_reconfigure[];
extern const char *__wt_confdfl_session_rename;
extern WT_CONFIG_CHECK __wt_confchk_session_rename[];
extern const char *__wt_confdfl_session_rollback_transaction;
extern WT_CONFIG_CHECK __wt_confchk_session_rollback_transaction[];
extern const char *__wt_confdfl_session_salvage;
extern WT_CONFIG_CHECK __wt_confchk_session_salvage[];
extern const char *__wt_confdfl_session_truncate;
extern WT_CONFIG_CHECK __wt_confchk_session_truncate[];
extern const char *__wt_confdfl_session_upgrade;
extern WT_CONFIG_CHECK __wt_confchk_session_upgrade[];
extern const char *__wt_confdfl_session_verify;
extern WT_CONFIG_CHECK __wt_confchk_session_verify[];
extern const char *__wt_confdfl_table_meta;
extern WT_CONFIG_CHECK __wt_confchk_table_meta[];
extern const char *__wt_confdfl_wiredtiger_open;
extern WT_CONFIG_CHECK __wt_confchk_checkpoint_subconfigs[];
extern WT_CONFIG_CHECK __wt_confchk_statistics_log_subconfigs[];
extern WT_CONFIG_CHECK __wt_confchk_wiredtiger_open[];
extern int __wt_conn_btree_sync_and_close(WT_SESSION_IMPL *session);
extern int __wt_conn_btree_get(WT_SESSION_IMPL *session,
    const char *name,
    const char *ckpt,
    const char *op_cfg[],
    uint32_t flags);
extern int __wt_conn_btree_apply(WT_SESSION_IMPL *session,
    int (*func)(WT_SESSION_IMPL *,
    const char *[]),
    const char *cfg[]);
extern int __wt_conn_btree_apply_single(WT_SESSION_IMPL *session,
    const char *uri,
    int (*func)(WT_SESSION_IMPL *,
    const char *[]),
    const char *cfg[]);
extern int __wt_conn_btree_close(WT_SESSION_IMPL *session, int locked);
extern int __wt_conn_dhandle_close_all(WT_SESSION_IMPL *session,
    const char *name);
extern int __wt_conn_dhandle_discard_single( WT_SESSION_IMPL *session,
    WT_DATA_HANDLE *dhandle);
extern int __wt_conn_dhandle_discard(WT_CONNECTION_IMPL *conn);
extern int __wt_cache_config(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern int __wt_cache_create(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern void __wt_cache_stats_update(WT_SESSION_IMPL *session);
extern int __wt_cache_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_conn_cache_pool_config(WT_SESSION_IMPL *session,
    const char **cfg);
extern int __wt_conn_cache_pool_open(WT_SESSION_IMPL *session);
extern int __wt_conn_cache_pool_destroy(WT_CONNECTION_IMPL *conn);
extern void *__wt_cache_pool_server(void *arg);
extern int __wt_checkpoint_create(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern int __wt_checkpoint_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_init(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern int __wt_connection_close(WT_CONNECTION_IMPL *conn);
extern void __wt_conn_stat_init(WT_SESSION_IMPL *session, uint32_t flags);
extern int __wt_statlog_create(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern int __wt_statlog_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_curbackup_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curbulk_init(WT_CURSOR_BULK *cbulk, int bitmap);
extern int __wt_curconfig_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curdump_create(WT_CURSOR *child,
    WT_CURSOR *owner,
    WT_CURSOR **cursorp);
extern int __wt_curfile_truncate( WT_SESSION_IMPL *session,
    WT_CURSOR *start,
    WT_CURSOR *stop);
extern int __wt_curfile_create(WT_SESSION_IMPL *session,
    WT_CURSOR *owner,
    const char *cfg[],
    int bulk,
    int bitmap,
    WT_CURSOR **cursorp);
extern int __wt_curfile_open(WT_SESSION_IMPL *session,
    const char *uri,
    WT_CURSOR *owner,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curindex_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curstat_init(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR_STAT *cst,
    uint32_t flags);
extern int __wt_curstat_open(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_cursor_notsup(WT_CURSOR *cursor);
extern int __wt_cursor_noop(WT_CURSOR *cursor);
extern void __wt_cursor_set_notsup(WT_CURSOR *cursor);
extern int __wt_cursor_kv_not_set(WT_CURSOR *cursor, int key);
extern int __wt_cursor_get_key(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_key(WT_CURSOR *cursor, ...);
extern int __wt_cursor_get_raw_key(WT_CURSOR *cursor, WT_ITEM *key);
extern void __wt_cursor_set_raw_key(WT_CURSOR *cursor, WT_ITEM *key);
extern int __wt_cursor_get_keyv(WT_CURSOR *cursor, uint32_t flags, va_list ap);
extern void __wt_cursor_set_keyv(WT_CURSOR *cursor, uint32_t flags, va_list ap);
extern int __wt_cursor_get_value(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_value(WT_CURSOR *cursor, ...);
extern int __wt_cursor_close(WT_CURSOR *cursor);
extern int __wt_cursor_dup(WT_SESSION_IMPL *session,
    WT_CURSOR *to_dup,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_cursor_init(WT_CURSOR *cursor,
    const char *uri,
    WT_CURSOR *owner,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_curtable_get_key(WT_CURSOR *cursor, ...);
extern int __wt_curtable_get_value(WT_CURSOR *cursor, ...);
extern void __wt_curtable_set_key(WT_CURSOR *cursor, ...);
extern void __wt_curtable_set_value(WT_CURSOR *cursor, ...);
extern int __wt_curtable_truncate( WT_SESSION_IMPL *session,
    WT_CURSOR *start,
    WT_CURSOR *stop);
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
extern int __wt_clsm_init_merge( WT_CURSOR *cursor,
    u_int start_chunk,
    uint32_t start_id,
    u_int nchunks);
extern int __wt_clsm_open(WT_SESSION_IMPL *session,
    const char *uri,
    WT_CURSOR *owner,
    const char *cfg[],
    WT_CURSOR **cursorp);
extern int __wt_lsm_init(WT_CONNECTION *wt_conn, const char *config);
extern int __wt_lsm_cleanup(WT_CONNECTION *wt_conn);
extern int __wt_lsm_merge_update_tree(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree,
    u_int start_chunk,
    u_int nchunks,
    WT_LSM_CHUNK *chunk);
extern int __wt_lsm_merge( WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree,
    u_int id,
    int stalls);
extern int __wt_lsm_meta_read(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern int __wt_lsm_meta_write(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern int __wt_lsm_stat_init(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree,
    WT_CURSOR_STAT *cst,
    uint32_t flags);
extern int __wt_lsm_tree_close_all(WT_SESSION_IMPL *session);
extern int __wt_lsm_tree_bloom_name( WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree,
    uint32_t id,
    WT_ITEM *buf);
extern int __wt_lsm_tree_chunk_name( WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree,
    uint32_t id,
    WT_ITEM *buf);
extern int __wt_lsm_tree_setup_chunk( WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree,
    WT_LSM_CHUNK *chunk);
extern int __wt_lsm_tree_create(WT_SESSION_IMPL *session,
    const char *uri,
    int exclusive,
    const char *config);
extern int __wt_lsm_tree_get(WT_SESSION_IMPL *session,
    const char *uri,
    int exclusive,
    WT_LSM_TREE **treep);
extern void __wt_lsm_tree_release(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree);
extern int __wt_lsm_tree_switch( WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree);
extern int __wt_lsm_tree_drop( WT_SESSION_IMPL *session,
    const char *name,
    const char *cfg[]);
extern int __wt_lsm_tree_rename(WT_SESSION_IMPL *session,
    const char *oldname,
    const char *newname,
    const char *cfg[]);
extern int __wt_lsm_tree_truncate( WT_SESSION_IMPL *session,
    const char *name,
    const char *cfg[]);
extern int __wt_lsm_tree_worker(WT_SESSION_IMPL *session,
    const char *uri,
    int (*func)(WT_SESSION_IMPL *,
    const char *[]),
    const char *cfg[],
    uint32_t open_flags);
extern void *__wt_lsm_merge_worker(void *vargs);
extern void *__wt_lsm_bloom_worker(void *arg);
extern void *__wt_lsm_checkpoint_worker(void *arg);
extern int __wt_metadata_get(WT_SESSION *session,
    const char *uri,
    const char **valuep);
extern int __wt_metadata_get_ckptlist( WT_SESSION *session,
    const char *name,
    WT_CKPT **ckptbasep);
extern void __wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase);
extern int __wt_meta_btree_apply(WT_SESSION_IMPL *session,
    int (*func)(WT_SESSION_IMPL *,
    const char *[]),
    const char *cfg[]);
extern int __wt_meta_checkpoint(WT_SESSION_IMPL *session,
    const char *fname,
    const char *checkpoint,
    WT_CKPT *ckpt);
extern int __wt_meta_checkpoint_last_name( WT_SESSION_IMPL *session,
    const char *fname,
    const char **namep);
extern int __wt_meta_checkpoint_clear(WT_SESSION_IMPL *session,
    const char *fname);
extern int __wt_meta_ckptlist_get( WT_SESSION_IMPL *session,
    const char *fname,
    WT_CKPT **ckptbasep);
extern int __wt_meta_ckptlist_set( WT_SESSION_IMPL *session,
    const char *fname,
    WT_CKPT *ckptbase);
extern void __wt_meta_ckptlist_free(WT_SESSION_IMPL *session,
    WT_CKPT *ckptbase);
extern void __wt_meta_checkpoint_free(WT_SESSION_IMPL *session, WT_CKPT *ckpt);
extern int __wt_metadata_open(WT_SESSION_IMPL *session);
extern int __wt_metadata_load_backup(WT_SESSION_IMPL *session);
extern int __wt_metadata_cursor( WT_SESSION_IMPL *session,
    const char *config,
    WT_CURSOR **cursorp);
extern int __wt_metadata_insert( WT_SESSION_IMPL *session,
    const char *key,
    const char *value);
extern int __wt_metadata_update( WT_SESSION_IMPL *session,
    const char *key,
    const char *value);
extern int __wt_metadata_remove(WT_SESSION_IMPL *session, const char *key);
extern int __wt_metadata_read( WT_SESSION_IMPL *session,
    const char *key,
    const char **valuep);
extern void __wt_meta_track_discard(WT_SESSION_IMPL *session);
extern int __wt_meta_track_on(WT_SESSION_IMPL *session);
extern int __wt_meta_track_off(WT_SESSION_IMPL *session, int unroll);
extern int __wt_meta_track_sub_on(WT_SESSION_IMPL *session);
extern int __wt_meta_track_sub_off(WT_SESSION_IMPL *session);
extern int __wt_meta_track_checkpoint(WT_SESSION_IMPL *session);
extern int __wt_meta_track_insert(WT_SESSION_IMPL *session, const char *key);
extern int __wt_meta_track_update(WT_SESSION_IMPL *session, const char *key);
extern int __wt_meta_track_fileop( WT_SESSION_IMPL *session,
    const char *olduri,
    const char *newuri);
extern int __wt_meta_track_handle_lock(WT_SESSION_IMPL *session, int created);
extern int __wt_meta_turtle_init(WT_SESSION_IMPL *session, int *existp);
extern int __wt_meta_turtle_read( WT_SESSION_IMPL *session,
    const char *key,
    const char **valuep);
extern int __wt_meta_turtle_update( WT_SESSION_IMPL *session,
    const char *key,
    const char *value);
extern void __wt_abort(WT_SESSION_IMPL *session) WT_GCC_ATTRIBUTE((noreturn));
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
extern int __wt_getline(WT_SESSION_IMPL *session, WT_ITEM *buf, FILE *fp);
extern int __wt_mmap(WT_SESSION_IMPL *session,
    WT_FH *fh,
    void *mapp,
    size_t *lenp);
extern int __wt_mmap_read(WT_SESSION_IMPL *session, void *p, size_t size);
extern int __wt_mmap_discard(WT_SESSION_IMPL *session, void *p, size_t size);
extern int __wt_munmap(WT_SESSION_IMPL *session,
    WT_FH *fh,
    void *map,
    size_t len);
extern int __wt_cond_alloc(WT_SESSION_IMPL *session,
    const char *name,
    int is_signalled,
    WT_CONDVAR **condp);
extern int __wt_cond_wait(WT_SESSION_IMPL *session,
    WT_CONDVAR *cond,
    long usecs);
extern int __wt_cond_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond);
extern int __wt_cond_destroy(WT_SESSION_IMPL *session, WT_CONDVAR **condp);
extern int __wt_rwlock_alloc( WT_SESSION_IMPL *session,
    const char *name,
    WT_RWLOCK **rwlockp);
extern int __wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock);
extern int __wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp);
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
extern uint64_t __wt_strtouq(const char *nptr, char **endptr, int base);
extern int __wt_thread_create(WT_SESSION_IMPL *session,
    pthread_t *tidret,
    void *(*func)(void *),
    void *arg);
extern int __wt_thread_join(WT_SESSION_IMPL *session, pthread_t tid);
extern int __wt_epoch(WT_SESSION_IMPL *session, struct timespec *tsp);
extern void __wt_yield(void);
extern int __wt_struct_check(WT_SESSION_IMPL *session,
    const char *fmt,
    size_t len,
    int *fixedp,
    uint32_t *fixed_lenp);
extern int __wt_struct_sizev( WT_SESSION_IMPL *session,
    size_t *sizep,
    const char *fmt,
    va_list ap);
extern int __wt_struct_size(WT_SESSION_IMPL *session,
    size_t *sizep,
    const char *fmt,
    ...);
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
extern int __wt_schema_colgroup_source(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *cgname,
    const char *config,
    WT_ITEM *buf);
extern int __wt_schema_index_source(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *idxname,
    const char *config,
    WT_ITEM *buf);
extern int __wt_schema_create( WT_SESSION_IMPL *session,
    const char *name,
    const char *config);
extern int __wt_schema_drop(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[]);
extern int __wt_schema_get_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    int ok_incomplete,
    WT_TABLE **tablep);
extern void __wt_schema_release_table(WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern void __wt_schema_destroy_colgroup(WT_SESSION_IMPL *session,
    WT_COLGROUP *colgroup);
extern void __wt_schema_destroy_index(WT_SESSION_IMPL *session, WT_INDEX *idx);
extern void __wt_schema_destroy_table(WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern void __wt_schema_remove_table( WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern void __wt_schema_close_tables(WT_SESSION_IMPL *session);
extern int __wt_schema_colgroup_name(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *cgname,
    size_t len,
    WT_ITEM *buf);
extern int __wt_schema_open_colgroups(WT_SESSION_IMPL *session,
    WT_TABLE *table);
extern int __wt_schema_open_index(WT_SESSION_IMPL *session,
    WT_TABLE *table,
    const char *idxname,
    size_t len,
    WT_INDEX **indexp);
extern int __wt_schema_open_indices(WT_SESSION_IMPL *session, WT_TABLE *table);
extern int __wt_schema_open_table(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    WT_TABLE **tablep);
extern int __wt_schema_get_colgroup(WT_SESSION_IMPL *session,
    const char *uri,
    WT_TABLE **tablep,
    WT_COLGROUP **colgroupp);
extern int __wt_schema_get_index(WT_SESSION_IMPL *session,
    const char *uri,
    WT_TABLE **tablep,
    WT_INDEX **indexp);
extern int __wt_schema_colcheck(WT_SESSION_IMPL *session,
    const char *key_format,
    const char *value_format,
    WT_CONFIG_ITEM *colconf,
    u_int *kcolsp,
    u_int *vcolsp);
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
extern int __wt_schema_stat_init(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    WT_CURSOR_STAT *cst,
    uint32_t flags);
extern int __wt_schema_truncate( WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[]);
extern int __wt_schema_get_source( WT_SESSION_IMPL *session,
    const char *name,
    WT_DATA_SOURCE **dsrcp);
extern int __wt_schema_name_check(WT_SESSION_IMPL *session, const char *uri);
extern int __wt_schema_worker(WT_SESSION_IMPL *session,
    const char *uri,
    int (*func)(WT_SESSION_IMPL *,
    const char *[]),
    const char *cfg[],
    uint32_t open_flags);
extern int __wt_open_cursor(WT_SESSION_IMPL *session,
    const char *uri,
    WT_CURSOR *owner,
    const char *cfg[],
    WT_CURSOR **cursorp);
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
    WT_DATA_HANDLE_CACHE **dhandle_cachep);
extern int __wt_session_lock_btree(WT_SESSION_IMPL *session, uint32_t flags);
extern int __wt_session_release_btree(WT_SESSION_IMPL *session);
extern int __wt_session_get_btree_ckpt(WT_SESSION_IMPL *session,
    const char *uri,
    const char *cfg[],
    uint32_t flags);
extern int __wt_session_get_btree(WT_SESSION_IMPL *session,
    const char *uri,
    const char *checkpoint,
    const char *cfg[],
    uint32_t flags);
extern int __wt_session_lock_checkpoint(WT_SESSION_IMPL *session,
    const char *checkpoint);
extern int __wt_session_discard_btree( WT_SESSION_IMPL *session,
    WT_DATA_HANDLE_CACHE *dhandle_cache);
extern int __wt_salvage(WT_SESSION_IMPL *session, const char *cfg[]);
extern uint32_t __wt_cksum(const void *chunk, size_t len);
extern void __wt_event_handler_set(WT_SESSION_IMPL *session,
    WT_EVENT_HANDLER *handler);
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
extern int __wt_verrx(WT_SESSION_IMPL *session, const char *fmt, va_list ap);
extern int __wt_msg(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    2,
    3)));
extern int __wt_vmsg(WT_SESSION_IMPL *session, const char *fmt, va_list ap);
extern int __wt_progress(WT_SESSION_IMPL *session, const char *s, uint64_t v);
extern int __wt_verbose(WT_SESSION_IMPL *session,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    2,
    3)));
extern void __wt_assert(WT_SESSION_IMPL *session,
    int error,
    const char *file_name,
    int line_number,
    const char *fmt,
    ...) WT_GCC_ATTRIBUTE((format (printf,
    5,
    6)));
extern int __wt_panic(WT_SESSION_IMPL *session);
extern int __wt_illegal_value(WT_SESSION_IMPL *session, const char *name);
extern int __wt_bad_object_type(WT_SESSION_IMPL *session, const char *uri);
extern int __wt_absolute_path(const char *path);
extern int __wt_filename(WT_SESSION_IMPL *session,
    const char *name,
    const char **path);
extern int __wt_nfilename(WT_SESSION_IMPL *session,
    const char *name,
    size_t namelen,
    const char **path);
extern int __wt_library_init(void);
extern int __wt_breakpoint(void);
extern void __wt_attach(WT_SESSION_IMPL *session);
extern uint64_t __wt_hash_city64(const void *string, uint32_t len);
extern uint64_t __wt_hash_fnv64(const void *string, uint32_t len);
extern int
__wt_hazard_set(WT_SESSION_IMPL *session, WT_REF *ref, int *busyp
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern int __wt_hazard_clear(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_hazard_close(WT_SESSION_IMPL *session);
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
extern int
__wt_scr_alloc_func(WT_SESSION_IMPL *session,
 size_t size, WT_ITEM **scratchp
#ifdef HAVE_DIAGNOSTIC
 , const char *file, int line
#endif
 );
extern void __wt_scr_free(WT_ITEM **bufp);
extern void __wt_scr_discard(WT_SESSION_IMPL *session);
extern void *__wt_scr_alloc_ext(WT_SESSION *wt_session, size_t size);
extern void __wt_scr_free_ext(WT_SESSION *wt_session, void *p);
extern void __wt_session_dump_all(WT_SESSION_IMPL *session);
extern void __wt_session_dump(WT_SESSION_IMPL *session);
extern void __wt_stat_init_dsrc_stats(WT_DSRC_STATS *stats);
extern void __wt_stat_clear_dsrc_stats(void *stats_arg);
extern void __wt_stat_init_connection_stats(WT_CONNECTION_STATS *stats);
extern void __wt_stat_clear_connection_stats(void *stats_arg);
extern int __wt_txnid_cmp(const void *v1, const void *v2);
extern void __wt_txn_release_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_get_oldest(WT_SESSION_IMPL *session);
extern void __wt_txn_get_snapshot( WT_SESSION_IMPL *session,
    wt_txnid_t my_id,
    wt_txnid_t max_id,
    int force);
extern void __wt_txn_get_evict_snapshot(WT_SESSION_IMPL *session);
extern int __wt_txn_begin(WT_SESSION_IMPL *session, const char *cfg[]);
extern void __wt_txn_release(WT_SESSION_IMPL *session);
extern int __wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_txn_init(WT_SESSION_IMPL *session);
extern void __wt_txn_destroy(WT_SESSION_IMPL *session);
extern int __wt_txn_global_init(WT_CONNECTION_IMPL *conn, const char *cfg[]);
extern void __wt_txn_global_destroy(WT_CONNECTION_IMPL *conn);
extern int __wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[]);
extern int __wt_checkpoint_close(WT_SESSION_IMPL *session, const char *cfg[]);
