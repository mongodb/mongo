extern WT_DATA_SOURCE *__wt_schema_get_source(WT_SESSION_IMPL *session, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern WT_HAZARD *__wt_hazard_check(WT_SESSION_IMPL *session, WT_REF *ref,
  WT_SESSION_IMPL **sessionp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern WT_THREAD_RET __wt_cache_pool_server(void *arg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern WT_UPDATE *__wt_update_obsolete_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *upd, bool update_accounting) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_block_offset_invalid(WT_BLOCK *block, wt_off_t offset, uint32_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_btree_immediately_durable(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_cell_type_check(uint8_t cell_type, uint8_t dsk_type)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_checksum_alt_match(const void *chunk, size_t len, uint32_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_curhs_check_insert_success(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_delete_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, bool visible_all)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_evict_thread_chk(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_fsync_background_chk(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_gen_active(WT_SESSION_IMPL *session, int which, uint64_t generation)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_handle_is_open(WT_SESSION_IMPL *session, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_hazard_check_assert(WT_SESSION_IMPL *session, void *ref, bool waitfor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_ispo2(uint32_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_lsm_chunk_visible_all(WT_SESSION_IMPL *session, WT_LSM_CHUNK *chunk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_modify_idempotent(const void *modify)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_page_evict_urgent(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_read_cell_time_window(WT_CURSOR_BTREE *cbt, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_rwlock_islocked(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_txn_active(WT_SESSION_IMPL *session, uint64_t txnid)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern char *__wt_time_aggregate_to_string(WT_TIME_AGGREGATE *ta, char *ta_string)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern char *__wt_time_point_to_string(wt_timestamp_t ts, wt_timestamp_t durable_ts,
  uint64_t txn_id, char *tp_string) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern char *__wt_time_window_to_string(WT_TIME_WINDOW *tw, char *tw_string)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern char *__wt_timestamp_to_string(wt_timestamp_t ts, char *ts_string)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const WT_CONFIG_ENTRY *__wt_conn_config_match(const char *method)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const WT_CONFIG_ENTRY *__wt_test_config_match(const char *test_name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_addr_string(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_buf_set_printable(WT_SESSION_IMPL *session, const void *p, size_t size,
  bool hexonly, WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_buf_set_printable_format(WT_SESSION_IMPL *session, const void *buffer,
  size_t size, const char *format, bool hexonly, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_buf_set_size(WT_SESSION_IMPL *session, uint64_t size, bool exact,
  WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_cell_type_string(uint8_t type)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_ext_strerror(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, int error)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_json_tokname(int toktype) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_key_string(WT_SESSION_IMPL *session, const void *data_arg, size_t size,
  const char *key_format, WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_page_type_string(u_int type) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_session_strerror(WT_SESSION *wt_session, int error)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_strerror(WT_SESSION_IMPL *session, int error, char *errbuf, size_t errlen)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern const char *__wt_wiredtiger_error(int error)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_apply_single_idx(WT_SESSION_IMPL *session, WT_INDEX *idx, WT_CURSOR *cur,
  WT_CURSOR_TABLE *ctable, int (*f)(WT_CURSOR *)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_file_remove(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_load_incr(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *blkcfg,
  WT_ITEM *bitstring, uint64_t nbits) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bad_object_type(WT_SESSION_IMPL *session, const char *uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_map(WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapped_regionp,
  size_t *lengthp, void *mapped_cookiep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_map_read(WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr,
  size_t addr_size, bool *foundp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_put(WT_SESSION_IMPL *session, WT_ITEM *data, const uint8_t *addr,
  size_t addr_size, bool write) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_read(WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_unmap(WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapped_region,
  size_t length, void *mapped_cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_write(WT_SESSION_IMPL *session, WT_ITEM *buf, uint8_t *addr,
  size_t *addr_sizep, size_t *compressed_sizep, bool checkpoint, bool checkpoint_io,
  bool compressed) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_invalid(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size, bool live) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_pack(WT_BLOCK *block, uint8_t **pp, uint32_t objectid, wt_off_t offset,
  uint32_t size, uint32_t checksum) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_string(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  const uint8_t *addr, size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_unpack(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *p,
  size_t addr_size, uint32_t *objectidp, wt_off_t *offsetp, uint32_t *sizep, uint32_t *checksump)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_alloc(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t *offp,
  wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_cache_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  WT_CKPT *ckptbase, bool data_checksum) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint_final(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  uint8_t **file_sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint_last(WT_SESSION_IMPL *session, WT_BLOCK *block, char **metadatap,
  char **checkpoint_listp, WT_ITEM *checkpoint) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint_load(WT_SESSION_IMPL *session, WT_BLOCK *block,
  const uint8_t *addr, size_t addr_size, uint8_t *root_addr, size_t *root_addr_sizep,
  bool checkpoint) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint_resolve(WT_SESSION_IMPL *session, WT_BLOCK *block, bool failed)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint_unload(WT_SESSION_IMPL *session, WT_BLOCK *block, bool checkpoint)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ckpt_decode(WT_SESSION *wt_session, WT_BLOCK *block, const uint8_t *ckpt,
  size_t ckpt_size, WT_BLOCK_CKPT *ci) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ckpt_init(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ckpt_pack(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t **pp,
  WT_BLOCK_CKPT *ci, bool skip_avail) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ckpt_unpack(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *ckpt,
  size_t ckpt_size, WT_BLOCK_CKPT *ci) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_compact_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_compact_page_rewrite(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr,
  size_t *addr_sizep, bool *skipp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_compact_page_skip(
  WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_compact_skip(WT_SESSION_IMPL *session, WT_BLOCK *block, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_compact_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_discard(WT_SESSION_IMPL *session, WT_BLOCK *block, size_t added_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ext_alloc(WT_SESSION_IMPL *session, WT_EXT **extp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ext_discard(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_ext_prealloc(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_check(WT_SESSION_IMPL *session, WT_EXTLIST *al, WT_EXTLIST *bl)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_init(WT_SESSION_IMPL *session, WT_EXTLIST *el, const char *name,
  const char *extname, bool track_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_merge(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *a,
  WT_EXTLIST *b) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_read(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t ckpt_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_read_avail(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t ckpt_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_extlist_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  WT_EXTLIST *additional) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_fh(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t object_id, WT_FH **fhp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_free(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_insert_ext(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t off, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_create(WT_SESSION_IMPL *session, const char *filename,
  uint32_t allocsize) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_drop(WT_SESSION_IMPL *session, const char *filename, bool durable)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_named_size(WT_SESSION_IMPL *session, const char *name,
  wt_off_t *sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_open(WT_SESSION_IMPL *session, const char *filename,
  WT_BLOCK_FILE_OPENER *opener, const char *cfg[], bool forced_salvage, bool readonly,
  uint32_t allocsize, WT_BM **bmp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_size(WT_BM *bm, WT_SESSION_IMPL *session, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_misplaced(WT_SESSION_IMPL *session, WT_BLOCK *block, const char *list,
  wt_off_t offset, uint32_t size, bool live, const char *func, int line)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_off_free(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t objectid,
  wt_off_t offset, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_off_remove_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t off, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_open(WT_SESSION_IMPL *session, const char *filename,
  WT_BLOCK_FILE_OPENER *opener, const char *cfg[], bool forced_salvage, bool readonly,
  uint32_t allocsize, WT_BLOCK **blockp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_read_off(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  uint32_t objectid, wt_off_t offset, uint32_t size, uint32_t checksum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_read_off_blind(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t offset,
  uint32_t *sizep, uint32_t *checksump) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_salvage_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_salvage_next(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr,
  size_t *addr_sizep, bool *eofp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_salvage_start(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_salvage_valid(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t *addr,
  size_t addr_size, bool valid) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_switch_object(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t object_id,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_verify_addr(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_verify_start(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase,
  const char *cfg[]) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, uint8_t *addr,
  size_t *addr_sizep, bool data_checksum, bool checkpoint_io)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_write_off(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  uint32_t *objectidp, wt_off_t *offsetp, uint32_t *sizep, uint32_t *checksump, bool data_checksum,
  bool checkpoint_io, bool caller_locked) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_write_size(WT_SESSION_IMPL *session, WT_BLOCK *block, size_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_close(WT_BLOOM *bloom) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_create(WT_SESSION_IMPL *session, const char *uri, const char *config,
  uint64_t count, uint32_t factor, uint32_t k, WT_BLOOM **bloomp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_drop(WT_BLOOM *bloom, const char *config) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_finalize(WT_BLOOM *bloom) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_get(WT_BLOOM *bloom, WT_ITEM *key) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_hash_get(WT_BLOOM *bloom, WT_BLOOM_HASH *bhash)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_inmem_get(WT_BLOOM *bloom, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_intersection(WT_BLOOM *bloom, WT_BLOOM *other)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bloom_open(WT_SESSION_IMPL *session, const char *uri, uint32_t factor, uint32_t k,
  WT_CURSOR *owner, WT_BLOOM **bloomp) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bm_corrupt(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bm_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_close(WT_CURSOR_BTREE *cbt, bool lowlevel)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_compare(WT_CURSOR_BTREE *a_arg, WT_CURSOR_BTREE *b_arg, int *cmpp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_equals(WT_CURSOR_BTREE *a_arg, WT_CURSOR_BTREE *b_arg, int *equalp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_insert(WT_CURSOR_BTREE *cbt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_insert_check(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_modify(WT_CURSOR_BTREE *cbt, WT_MODIFY *entries, int nentries)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_next(WT_CURSOR_BTREE *cbt, bool truncating)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_next_prefix(WT_CURSOR_BTREE *cbt, WT_ITEM *prefix, bool truncating)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_next_random(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_prev(WT_CURSOR_BTREE *cbt, bool truncating)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_range_truncate(WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_remove(WT_CURSOR_BTREE *cbt, bool positioned)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_reserve(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_reset(WT_CURSOR_BTREE *cbt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_search(WT_CURSOR_BTREE *cbt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exactp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_search_prepared(WT_CURSOR *cursor, WT_UPDATE **updp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_update(WT_CURSOR_BTREE *cbt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_config_encryptor(WT_SESSION_IMPL *session, const char **cfg,
  WT_KEYED_ENCRYPTOR **kencryptorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_discard(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_huffman_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_new_leaf_page(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_open(WT_SESSION_IMPL *session, const char *op_cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_stat_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_switch_object(WT_SESSION_IMPL *session, uint32_t object_id, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_tree_open(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_buf_catfmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
      WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_buf_fmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
      WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_buf_grow_worker(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_init(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_fix(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_fix_bitmap(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_row(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_var(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_wrapup(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_eviction_worker(WT_SESSION_IMPL *session, bool busy, bool readonly,
  double pct_full) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_pool_config(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_calc_modify(WT_SESSION_IMPL *wt_session, const WT_ITEM *oldv, const WT_ITEM *newv,
  size_t maxdiff, WT_MODIFY *entries, int *nentriesp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_calloc(WT_SESSION_IMPL *session, size_t number, size_t size, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_capacity_server_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_capacity_server_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_close(WT_SESSION_IMPL *session, bool final)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_get_handles(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_reserved_session_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_reserved_session_init(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_server_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_server_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_sync(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ckpt_blkmod_to_meta(WT_SESSION_IMPL *session, WT_ITEM *buf, WT_CKPT *ckpt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_close(WT_SESSION_IMPL *session, WT_FH **fhp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_close_connection_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clsm_await_switch(WT_CURSOR_LSM *clsm)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clsm_close(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clsm_init_merge(WT_CURSOR *cursor, u_int start_chunk, uint32_t start_id,
  u_int nchunks) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clsm_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clsm_open_bulk(WT_CURSOR_LSM *clsm, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clsm_request_switch(WT_CURSOR_LSM *clsm)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_col_fix_read_auxheader(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_COL_FIX_AUXILIARY_HEADER *auxhdr) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_col_modify(WT_CURSOR_BTREE *cbt, uint64_t recno, const WT_ITEM *value,
  WT_UPDATE *upd_arg, u_int modify_type, bool exclusive
#ifdef HAVE_DIAGNOSTIC
  ,
  bool restore
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_col_search(WT_CURSOR_BTREE *cbt, uint64_t search_recno, WT_REF *leaf,
  bool leaf_safe, bool *leaf_foundp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_collator_config(WT_SESSION_IMPL *session, const char *uri, WT_CONFIG_ITEM *cname,
  WT_CONFIG_ITEM *metadata, WT_COLLATOR **collatorp, int *ownp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_compact(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_compressor_config(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval,
  WT_COMPRESSOR **compressorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cond_auto_alloc(WT_SESSION_IMPL *session, const char *name, uint64_t min,
  uint64_t max, WT_CONDVAR **condp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_check(WT_SESSION_IMPL *session, const WT_CONFIG_ENTRY *entry,
  const char *config, size_t config_len) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_collapse(WT_SESSION_IMPL *session, const char **cfg, char **config_ret)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_get(WT_SESSION_IMPL *session, const char **cfg_arg, WT_CONFIG_ITEM *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_getone(WT_SESSION_IMPL *session, const char *config, WT_CONFIG_ITEM *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_getones(WT_SESSION_IMPL *session, const char *config, const char *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_getones_none(WT_SESSION_IMPL *session, const char *config, const char *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_gets(WT_SESSION_IMPL *session, const char **cfg, const char *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_gets_def(WT_SESSION_IMPL *session, const char **cfg, const char *key,
  int def, WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_gets_none(WT_SESSION_IMPL *session, const char **cfg, const char *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_merge(WT_SESSION_IMPL *session, const char **cfg, const char *cfg_strip,
  const char **config_ret) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_subget_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_subgetraw(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cfg, WT_CONFIG_ITEM *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_subgets(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cfg, const char *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_configure_method(WT_SESSION_IMPL *session, const char *method, const char *uri,
  const char *config, const char *type, const char *check)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_btree_apply(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_cache_pool_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_cache_pool_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_compat_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_config_init(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_alloc(WT_SESSION_IMPL *session, const char *uri,
  const char *checkpoint) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_close(WT_SESSION_IMPL *session, bool final, bool mark_dead)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_close_all(WT_SESSION_IMPL *session, const char *uri, bool removed,
  bool mark_dead) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_discard(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_discard_single(WT_SESSION_IMPL *session, bool final, bool mark_dead)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_find(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_open(WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_optrack_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_optrack_teardown(WT_SESSION_IMPL *session, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_remove_collator(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_remove_compressor(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_remove_data_source(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_remove_encryptor(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_remove_extractor(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_remove_storage_source(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_statistics_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_connection_close(WT_CONNECTION_IMPL *conn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_connection_init(WT_CONNECTION_IMPL *conn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_connection_workers(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_copy_and_sync(WT_SESSION *wt_session, const char *from, const char *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curbackup_free_incr(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curbackup_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curbackup_open_incr(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  WT_CURSOR *cursor, const char *cfg[], WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curbulk_close(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curbulk_init(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool bitmap,
  bool skip_sort_check) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curconfig_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curds_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_DATA_SOURCE *dsrc, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curdump_create(WT_CURSOR *child, WT_CURSOR *owner, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curfile_insert_check(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curfile_next_random(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curfile_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_cache(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_open(WT_SESSION_IMPL *session, WT_CURSOR *owner, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_search_near_after(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_search_near_before(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curindex_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curjoin_join(WT_SESSION_IMPL *session, WT_CURSOR_JOIN *cjoin, WT_INDEX *idx,
  WT_CURSOR *ref_cursor, uint8_t flags, uint8_t range, uint64_t count, uint32_t bloom_bit_count,
  uint32_t bloom_hash_count) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curjoin_joined(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curjoin_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curlog_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curmetadata_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_cache(WT_CURSOR *cursor, WT_DATA_HANDLE *dhandle)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_cache_get(WT_SESSION_IMPL *session, const char *uri, uint64_t hash_value,
  WT_CURSOR *to_dup, const char *cfg[], WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_cache_release(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool *released)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_cached(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_compare_notsup(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_copy_release_item(WT_CURSOR *cursor, WT_ITEM *item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_dup_position(WT_CURSOR *to_dup, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_equals(WT_CURSOR *cursor, WT_CURSOR *other, int *equalp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_equals_notsup(WT_CURSOR *cursor, WT_CURSOR *other, int *equalp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_key(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_keyv(WT_CURSOR *cursor, uint32_t flags, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_raw_key(WT_CURSOR *cursor, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_raw_value(WT_CURSOR *cursor, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_value(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_value_notsup(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_valuev(WT_CURSOR *cursor, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_init(WT_CURSOR *cursor, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_key_order_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_key_order_init(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_kv_not_set(WT_CURSOR *cursor, bool key) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_largest_key(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_modify_notsup(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_modify_value_format_notsup(WT_CURSOR *cursor, WT_MODIFY *entries,
  int nentries) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_noop(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_notsup(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_reconfigure(WT_CURSOR *cursor, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_reconfigure_notsup(WT_CURSOR *cursor, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_reopen_notsup(WT_CURSOR *cursor, bool check_only)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_search_near_notsup(WT_CURSOR *cursor, int *exact)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_set_keyv(WT_CURSOR *cursor, uint32_t flags, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_set_valuev(WT_CURSOR *cursor, const char *fmt, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_valid(WT_CURSOR_BTREE *cbt, WT_ITEM *key, uint64_t recno, bool *valid)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_colgroup_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_index_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_init(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *curjoin,
  const char *cfg[], WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_lsm_init(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR_STAT *cst)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_table_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curtable_get_key(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curtable_get_value(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curtable_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curversion_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_addr(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  const char *ofile) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_addr_print(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_cursor_page(void *cursor_arg, const char *ofile) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_cursor_tree_hs(void *session_arg, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_disk(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_mode_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_offset(WT_SESSION_IMPL *session, wt_off_t offset, uint32_t size,
  uint32_t checksum, const char *ofile) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_offset_blind(WT_SESSION_IMPL *session, wt_off_t offset, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_page(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_set_verbose(WT_SESSION_IMPL *session, const char *v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_tree(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_tree_all(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_tree_shape(WT_SESSION_IMPL *session, WT_REF *ref, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_decrypt(WT_SESSION_IMPL *session, WT_ENCRYPTOR *encryptor, size_t skip, WT_ITEM *in,
  WT_ITEM *out) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_delete_page(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_delete_page_instantiate(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_delete_page_rollback(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_desc_write(WT_SESSION_IMPL *session, WT_FH *fh, uint32_t allocsize)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_direct_io_size_check(WT_SESSION_IMPL *session, const char **cfg,
  const char *config_name, uint32_t *allocsizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_encrypt(WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor, size_t skip,
  WT_ITEM *in, WT_ITEM *out) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_encryptor_config(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval,
  WT_CONFIG_ITEM *keyid, WT_CONFIG_ARG *cfg_arg, WT_KEYED_ENCRYPTOR **kencryptorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_errno(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_esc_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict(WT_SESSION_IMPL *session, WT_REF *ref, uint8_t previous_state, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict_file(WT_SESSION_IMPL *session, WT_CACHE_OP syncop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict_file_exclusive_on(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_evict_thread_stop(WT_SESSION_IMPL *session, WT_THREAD *thread)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_exclusive_handle_operation(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[], uint32_t open_flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_config_get(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  WT_CONFIG_ARG *cfg_arg, const char *key, WT_CONFIG_ITEM *cval)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_config_get_string(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const char *config, const char *key, WT_CONFIG_ITEM *cval)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_config_parser_open(WT_EXTENSION_API *wt_ext, WT_SESSION *wt_session,
  const char *config, size_t len, WT_CONFIG_PARSER **config_parserp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_config_parser_open_arg(WT_EXTENSION_API *wt_ext, WT_SESSION *wt_session,
  WT_CONFIG_ARG *cfg_arg, WT_CONFIG_PARSER **config_parserp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_err_printf(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *fmt,
  ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_map_windows_error(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  uint32_t windows_error) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_metadata_insert(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const char *key, const char *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_metadata_remove(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const char *key) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_metadata_search(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const char *key, char **valuep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_metadata_update(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const char *key, const char *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_msg_printf(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *fmt,
  ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_pack_close(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, size_t *usedp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_pack_int(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, int64_t i)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_pack_item(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, WT_ITEM *item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_pack_start(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *format,
  void *buffer, size_t size, WT_PACK_STREAM **psp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_pack_str(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, const char *s)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_pack_uint(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, uint64_t u)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_spin_init(WT_EXTENSION_API *wt_api, WT_EXTENSION_SPINLOCK *ext_spinlock,
  const char *name) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_struct_pack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, void *buffer,
  size_t len, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_struct_size(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, size_t *lenp,
  const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_struct_unpack(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const void *buffer, size_t len, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_unpack_int(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, int64_t *ip)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_unpack_item(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, WT_ITEM *item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_unpack_start(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session,
  const char *format, const void *buffer, size_t size, WT_PACK_STREAM **psp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_unpack_str(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, const char **sp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ext_unpack_uint(WT_EXTENSION_API *wt_api, WT_PACK_STREAM *ps, uint64_t *up)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_extractor_config(WT_SESSION_IMPL *session, const char *uri, const char *config,
  WT_EXTRACTOR **extractorp, int *ownp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_file_zero(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t start_off, wt_off_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_filename(WT_SESSION_IMPL *session, const char *name, char **path)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_filename_construct(WT_SESSION_IMPL *session, const char *path,
  const char *file_prefix, uintmax_t id_1, uint32_t id_2, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_flush_tier(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_fopen(WT_SESSION_IMPL *session, const char *name, uint32_t open_flags,
  uint32_t flags, WT_FSTREAM **fstrp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_fsync_background(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_getopt(const char *progname, int nargc, char *const *nargv, const char *ostr)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hazard_clear(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hazard_set_func(WT_SESSION_IMPL *session, WT_REF *ref, bool *busyp
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hex2byte(const u_char *from, u_char *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_config(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_delete_key_from_ts(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor,
  uint32_t btree_id, const WT_ITEM *key, wt_timestamp_t ts, bool reinsert, bool error_on_ooo_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_find_upd(WT_SESSION_IMPL *session, uint32_t btree_id, WT_ITEM *key,
  const char *value_format, uint64_t recno, WT_UPDATE_VALUE *upd_value, WT_ITEM *base_value_buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_get_btree(WT_SESSION_IMPL *session, WT_BTREE **hs_btreep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_insert_updates(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_MULTI *multi)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_modify(WT_CURSOR_BTREE *hs_cbt, WT_UPDATE *hs_upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_open(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_huffman_decode(WT_SESSION_IMPL *session, void *huffman_arg, const uint8_t *from_arg,
  size_t from_len, WT_ITEM *to_buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_huffman_encode(WT_SESSION_IMPL *session, void *huffman_arg, const uint8_t *from_arg,
  size_t from_len, WT_ITEM *to_buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_huffman_open(WT_SESSION_IMPL *session, void *symbol_frequency_array, u_int symcnt,
  u_int numbytes, void *retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_import_repair(WT_SESSION_IMPL *session, const char *uri, char **configp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_inmem_unsupported_op(WT_SESSION_IMPL *session, const char *tag)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_alloc_unpack(WT_SESSION_IMPL *session, const void *buffer, size_t size,
  const char *fmt, WT_CURSOR_JSON *json, bool iskey, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_column_init(WT_CURSOR *cursor, const char *uri, const char *keyformat,
  const WT_CONFIG_ITEM *idxconf, const WT_CONFIG_ITEM *colconf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_strncpy(WT_SESSION *wt_session, char **pdst, size_t dstlen, const char *src,
  size_t srclen) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_to_item(WT_SESSION_IMPL *session, const char *jstr, const char *format,
  WT_CURSOR_JSON *json, bool iskey, WT_ITEM *item) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_token(WT_SESSION *wt_session, const char *src, int *toktype,
  const char **tokstart, size_t *toklen) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_key_return(WT_CURSOR_BTREE *cbt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_library_init(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_acquire(WT_SESSION_IMPL *session, uint64_t recsize, WT_LOGSLOT *slot)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_allocfile(WT_SESSION_IMPL *session, uint32_t lognum, const char *dest)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_compat_verify(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_extract_lognum(WT_SESSION_IMPL *session, const char *name, uint32_t *id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_filename(WT_SESSION_IMPL *session, uint32_t id, const char *file_prefix,
  WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_fill(WT_SESSION_IMPL *session, WT_MYSLOT *myslot, bool force, WT_ITEM *record,
  WT_LSN *lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_flush(WT_SESSION_IMPL *session, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_flush_lsn(WT_SESSION_IMPL *session, WT_LSN *lsn, bool start)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_force_sync(WT_SESSION_IMPL *session, WT_LSN *min_lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_force_write(WT_SESSION_IMPL *session, bool retry, bool *did_work)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_get_backup_files(WT_SESSION_IMPL *session, char ***filesp, u_int *countp,
  uint32_t *maxid, bool active_only) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_needs_recovery(WT_SESSION_IMPL *session, WT_LSN *ckp_lsn, bool *recp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_open(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_printf(WT_SESSION_IMPL *session, const char *format, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_recover_system(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  WT_LSN *lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_release(WT_SESSION_IMPL *session, WT_LOGSLOT *slot, bool *freep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_remove(WT_SESSION_IMPL *session, const char *file_prefix, uint32_t lognum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_reset(WT_SESSION_IMPL *session, uint32_t lognum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *start_lsnp, WT_LSN *end_lsnp,
  uint32_t flags,
  int (*func)(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, WT_LSN *next_lsnp,
    void *cookie, int firstrecord),
  void *cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_set_version(WT_SESSION_IMPL *session, uint16_t version, uint32_t first_rec,
  bool downgrade, bool live_chg, uint32_t *lognump)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_slot_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_slot_init(WT_SESSION_IMPL *session, bool alloc)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_slot_switch(WT_SESSION_IMPL *session, WT_MYSLOT *myslot, bool retry,
  bool forced, bool *did_work) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_system_record(WT_SESSION_IMPL *session, WT_FH *log_fh, WT_LSN *lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_truncate_files(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool force)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_checkpoint_start_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_checkpoint_start_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_checkpoint_start_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_modify_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t recno, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_modify_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_put_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t recno, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_put_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_remove_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t recno) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_remove_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *recnop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_truncate_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t start, uint64_t stop) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_truncate_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *startp, uint64_t *stopp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_prev_lsn_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *prev_lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_prev_lsn_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_prev_lsn_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_LSN *prev_lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_read(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_modify_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *key, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_modify_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_put_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *key, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_put_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_remove_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *key) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_remove_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_truncate_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *start, WT_ITEM *stop, uint32_t mode) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_truncate_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *startp, WT_ITEM *stopp, uint32_t *modep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_txn_timestamp_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec,
  uint64_t time_sec, uint64_t time_nsec, uint64_t commit_ts, uint64_t durable_ts,
  uint64_t first_commit_ts, uint64_t prepare_ts, uint64_t read_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_txn_timestamp_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_txn_timestamp_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint64_t *time_secp, uint64_t *time_nsecp, uint64_t *commit_tsp,
  uint64_t *durable_tsp, uint64_t *first_commit_tsp, uint64_t *prepare_tsp, uint64_t *read_tsp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logrec_read(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *rectypep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_checkpoint_chunk(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  WT_LSM_CHUNK *chunk) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_compact(WT_SESSION_IMPL *session, const char *name, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_get_chunk_to_flush(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, bool force,
  WT_LSM_CHUNK **chunkp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_manager_config(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_manager_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_manager_pop_entry(WT_SESSION_IMPL *session, uint32_t type,
  WT_LSM_WORK_UNIT **entryp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_manager_push_entry(WT_SESSION_IMPL *session, uint32_t type, uint32_t flags,
  WT_LSM_TREE *lsm_tree) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_manager_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_manager_start(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_merge(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_merge_update_tree(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  u_int start_chunk, u_int nchunks, WT_LSM_CHUNK *chunk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_meta_read(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_meta_write(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  const char *newconfig) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_bloom_name(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t id,
  const char **retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_chunk_name(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t id,
  uint32_t generation, const char **retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_close_all(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_create(WT_SESSION_IMPL *session, const char *uri, bool exclusive,
  const char *config) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_drop(WT_SESSION_IMPL *session, const char *name, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_get(WT_SESSION_IMPL *session, const char *uri, bool exclusive,
  WT_LSM_TREE **treep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_rename(WT_SESSION_IMPL *session, const char *olduri, const char *newuri,
  const char *cfg[]) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_retire_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  u_int start_chunk, u_int nchunks) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_set_chunk_size(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  WT_LSM_CHUNK *chunk) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_setup_bloom(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  WT_LSM_CHUNK *chunk) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_setup_chunk(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree,
  WT_LSM_CHUNK *chunk) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_switch(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_truncate(WT_SESSION_IMPL *session, const char *name, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_tree_worker(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_work_bloom(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_work_enable_evict(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_work_switch(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT **entryp, bool *ran)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_worker_start(WT_SESSION_IMPL *session, WT_LSM_WORKER_ARGS *args)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_lsm_worker_stop(WT_SESSION_IMPL *session, WT_LSM_WORKER_ARGS *args)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_malloc(WT_SESSION_IMPL *session, size_t bytes_to_allocate, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_memdup(WT_SESSION_IMPL *session, const void *str, size_t len, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_apply_all(WT_SESSION_IMPL *session,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_block_metadata(WT_SESSION_IMPL *session, const char *config, WT_CKPT *ckpt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_checkpoint(WT_SESSION_IMPL *session, const char *fname, const char *checkpoint,
  WT_CKPT *ckpt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_checkpoint_clear(WT_SESSION_IMPL *session, const char *fname)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_checkpoint_last_name(WT_SESSION_IMPL *session, const char *fname,
  const char **namep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_get(WT_SESSION_IMPL *session, const char *fname, bool update,
  WT_CKPT **ckptbasep, size_t *allocated) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_get_from_config(WT_SESSION_IMPL *session, bool update,
  WT_CKPT **ckptbasep, size_t *allocatedp, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_set(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
  WT_CKPT *ckptbase, WT_LSN *ckptlsn) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_to_meta(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_update_config(WT_SESSION_IMPL *session, WT_CKPT *ckptbase,
  const char *oldcfg, char **newcfgp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_sysinfo_set(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_checkpoint(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_drop(WT_SESSION_IMPL *session, const char *filename)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_fileop(WT_SESSION_IMPL *session, const char *olduri, const char *newuri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_handle_lock(WT_SESSION_IMPL *session, bool created)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_init(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_insert(WT_SESSION_IMPL *session, const char *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_off(WT_SESSION_IMPL *session, bool need_sync, bool unroll)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_on(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_sub_off(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_update(WT_SESSION_IMPL *session, const char *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_correct_base_write_gen(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_cursor(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_cursor_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_cursor_open(WT_SESSION_IMPL *session, const char *config,
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_cursor_release(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_get_ckptlist(WT_SESSION *session, const char *name, WT_CKPT **ckptbasep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_init_base_write_gen(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_insert(WT_SESSION_IMPL *session, const char *key, const char *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_remove(WT_SESSION_IMPL *session, const char *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_search(WT_SESSION_IMPL *session, const char *key, char **valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_turtle_rewrite(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_update(WT_SESSION_IMPL *session, const char *key, const char *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_update_base_write_gen(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_apply_api(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_apply_item(WT_SESSION_IMPL *session, const char *value_format,
  WT_ITEM *value, const void *modify) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_pack(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries, WT_ITEM **modifyp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_reconstruct_from_upd_list(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *upd, WT_UPDATE_VALUE *upd_value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_msg(WT_SESSION_IMPL *session, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 2, 3)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_multi_to_ref(WT_SESSION_IMPL *session, WT_PAGE *page, WT_MULTI *multi,
  WT_REF **refp, size_t *incrp, bool closing) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_name_check(WT_SESSION_IMPL *session, const char *str, size_t len, bool check_uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_nfilename(WT_SESSION_IMPL *session, const char *name, size_t namelen, char **path)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_nhex_to_raw(WT_SESSION_IMPL *session, const char *from, size_t size, WT_ITEM *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_object_unsupported(WT_SESSION_IMPL *session, const char *uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_open(WT_SESSION_IMPL *session, const char *name, WT_FS_OPEN_FILE_TYPE file_type,
  u_int flags, WT_FH **fhp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_open_cursor(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_open_internal_session(WT_CONNECTION_IMPL *conn, const char *name,
  bool open_metadata, uint32_t session_flags, uint32_t session_lock_flags,
  WT_SESSION_IMPL **sessionp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_open_session(WT_CONNECTION_IMPL *conn, WT_EVENT_HANDLER *event_handler,
  const char *config, bool open_metadata, WT_SESSION_IMPL **sessionp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_os_inmemory(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_discard(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_discard_add(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_read(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_COMMON *unpack,
  WT_ITEM *store, bool *decoded) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_remove(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_KV *unpack)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_reuse_add(WT_SESSION_IMPL *session, WT_PAGE *page, const uint8_t *addr,
  size_t addr_size, const void *value, size_t value_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_reuse_search(WT_SESSION_IMPL *session, WT_PAGE *page, uint8_t **addrp,
  size_t *addr_sizep, const void *value, size_t value_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_track_init(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_track_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_alloc(WT_SESSION_IMPL *session, uint8_t type, uint32_t alloc_entries,
  bool alloc_refs, WT_PAGE **pagep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_in_func(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image, uint32_t flags,
  WT_PAGE **pagep, bool *preparedp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_inmem_prepare(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_modify_alloc(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_release_evict(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_panic_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 6, 7)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
      WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_progress(WT_SESSION_IMPL *session, const char *s, uint64_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_random_descent(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_range_truncate(WT_CURSOR *start, WT_CURSOR *stop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_raw_to_esc_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size,
  WT_ITEM *to) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_raw_to_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size, WT_ITEM *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_realloc(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret,
  size_t bytes_to_allocate, void *retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_realloc_aligned(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret,
  size_t bytes_to_allocate, void *retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_realloc_noclear(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret,
  size_t bytes_to_allocate, void *retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_cell_build_ovfl(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_KV *kv,
  uint8_t type, WT_TIME_WINDOW *tw, uint64_t rle) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_child_modify(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *ref,
  bool *hazardp, WT_CHILD_STATE *statep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_col_fix(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_col_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_col_var(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_dictionary_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, u_int slots)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_dictionary_lookup(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_KV *val,
  WT_REC_DICTIONARY **dpp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_hs_clear_on_tombstone(WT_SESSION_IMPL *session, WT_RECONCILE *r, uint64_t recno,
  WT_ITEM *rowkey) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_row_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_row_leaf(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref,
  WT_SALVAGE_COOKIE *salvage) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_split(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_split_crossing_bnd(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_split_finish(WT_SESSION_IMPL *session, WT_RECONCILE *r)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_split_grow(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t add_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_split_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page,
  uint64_t recno, uint64_t primary_size, uint32_t auxiliary_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rec_upd_select(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins,
  WT_ROW *rip, WT_CELL_UNPACK_KV *vpack, WT_UPDATE_SELECT *upd_select)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_reconcile(WT_SESSION_IMPL *session, WT_REF *ref, WT_SALVAGE_COOKIE *salvage,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_remove_if_exists(WT_SESSION_IMPL *session, const char *name, bool durable)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_reset_blkmod(WT_SESSION_IMPL *session, const char *orig_config, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[], bool no_ckpt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rollback_to_stable_one(WT_SESSION_IMPL *session, const char *uri, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_ikey(WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key,
  size_t size, WT_REF *ref) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_ikey_alloc(WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key,
  size_t size, WT_IKEY **ikeyp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_ikey_incr(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t cell_offset,
  const void *key, size_t size, WT_REF *ref) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_insert_alloc(WT_SESSION_IMPL *session, const WT_ITEM *key, u_int skipdepth,
  WT_INSERT **insp, size_t *ins_sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_leaf_key_copy(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip,
  WT_ITEM *key) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_leaf_key_work(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip_arg,
  WT_ITEM *keyb, bool instantiate) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_modify(WT_CURSOR_BTREE *cbt, const WT_ITEM *key, const WT_ITEM *value,
  WT_UPDATE *upd_arg, u_int modify_type, bool exclusive
#ifdef HAVE_DIAGNOSTIC
  ,
  bool restore
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_search(WT_CURSOR_BTREE *cbt, WT_ITEM *srch_key, bool insert, WT_REF *leaf,
  bool leaf_safe, bool *leaf_foundp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rts_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rwlock_init(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_salvage(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_alter(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_backup_check(WT_SESSION_IMPL *session, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_close_table(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_colcheck(WT_SESSION_IMPL *session, const char *key_format,
  const char *value_format, WT_CONFIG_ITEM *colconf, u_int *kcolsp, u_int *vcolsp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_colgroup_name(WT_SESSION_IMPL *session, WT_TABLE *table, const char *cgname,
  size_t len, WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_colgroup_source(
  WT_SESSION_IMPL *session, WT_TABLE *table, const char *cgname, const char *config, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_destroy_index(WT_SESSION_IMPL *session, WT_INDEX **idxp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_colgroup(WT_SESSION_IMPL *session, const char *uri, bool quiet,
  WT_TABLE **tablep, WT_COLGROUP **colgroupp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_index(WT_SESSION_IMPL *session, const char *uri, bool invalidate,
  bool quiet, WT_INDEX **indexp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_table(WT_SESSION_IMPL *session, const char *name, size_t namelen,
  bool ok_incomplete, uint32_t flags, WT_TABLE **tablep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_table_uri(WT_SESSION_IMPL *session, const char *uri, bool ok_incomplete,
  uint32_t flags, WT_TABLE **tablep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_tiered_uri(WT_SESSION_IMPL *session, const char *uri, uint32_t flags,
  WT_TIERED **tieredp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_index_source(WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname,
  const char *config, WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_internal_session(WT_SESSION_IMPL *session, WT_SESSION_IMPL **int_sessionp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_colgroups(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_index(WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname,
  size_t len, WT_INDEX **indexp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_indices(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_table(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_project_in(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_project_merge(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg,
  const char *vformat, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_project_out(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_project_slice(WT_SESSION_IMPL *session, WT_CURSOR **cp, const char *proj_arg,
  bool key_only, const char *vformat, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_range_truncate(WT_SESSION_IMPL *session, WT_CURSOR *start, WT_CURSOR *stop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_release_table(WT_SESSION_IMPL *session, WT_TABLE **tablep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_release_tiered(WT_SESSION_IMPL *session, WT_TIERED **tieredp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_rename(WT_SESSION_IMPL *session, const char *uri, const char *newuri,
  const char *cfg[]) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_session_release(WT_SESSION_IMPL *session, WT_SESSION_IMPL *int_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_tiered_worker(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_truncate(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_worker(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[], uint32_t open_flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_scr_alloc_func(WT_SESSION_IMPL *session, size_t size, WT_ITEM **scratchp
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_search_insert(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_INSERT_HEAD *ins_head, WT_ITEM *srch_key) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_breakpoint(WT_SESSION *wt_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_close_internal(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_compact(WT_SESSION *wt_session, const char *uri, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_compact_check_timeout(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_compact_readonly(WT_SESSION *wt_session, const char *uri,
  const char *config) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_copy_values(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_cursor_cache_sweep(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_get_btree_ckpt(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_get_dhandle(WT_SESSION_IMPL *session, const char *uri,
  const char *checkpoint, const char *cfg[], uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_lock_checkpoint(WT_SESSION_IMPL *session, const char *checkpoint)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_lock_dhandle(WT_SESSION_IMPL *session, uint32_t flags, bool *is_deadp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_notsup(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_range_truncate(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *start,
  WT_CURSOR *stop) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_release_dhandle(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_release_resources(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_reset_cursors(WT_SESSION_IMPL *session, bool free_buffers)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_set_return_func(WT_SESSION_IMPL *session, const char *func, int line, int err)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_insert(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_multi(WT_SESSION_IMPL *session, WT_REF *ref, int closing)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_reverse(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, WT_MULTI *multi)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stash_add(WT_SESSION_IMPL *session, int which, uint64_t generation, void *p,
  size_t len) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stat_connection_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stat_connection_init(WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *handle)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stat_dsrc_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stat_dsrc_init(WT_SESSION_IMPL *session, WT_DATA_HANDLE *handle)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stat_join_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_stat_session_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_statlog_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_statlog_destroy(WT_SESSION_IMPL *session, bool is_close)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_str_name_check(WT_SESSION_IMPL *session, const char *str)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_strndup(WT_SESSION_IMPL *session, const void *str, size_t len, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_check(WT_SESSION_IMPL *session, const char *fmt, size_t len, bool *fixedp,
  uint32_t *fixed_lenp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_confchk(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_pack(WT_SESSION_IMPL *session, void *buffer, size_t len, const char *fmt,
  ...) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_plan(WT_SESSION_IMPL *session, WT_TABLE *table, const char *columns,
  size_t len, bool value_only, WT_ITEM *plan) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_reformat(WT_SESSION_IMPL *session, WT_TABLE *table, const char *columns,
  size_t len, const char *extra_cols, bool value_only, WT_ITEM *format)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_repack(WT_SESSION_IMPL *session, const char *infmt, const char *outfmt,
  const WT_ITEM *inbuf, WT_ITEM *outbuf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_size(WT_SESSION_IMPL *session, size_t *lenp, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_truncate(WT_SESSION_IMPL *session, const char *input_fmt, u_int ncols,
  WT_ITEM *format) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_struct_unpack(WT_SESSION_IMPL *session, const void *buffer, size_t len,
  const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_sweep_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_sweep_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_sweep_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_sync_file(WT_SESSION_IMPL *session, WT_CACHE_OP syncop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_table_check(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_table_range_truncate(WT_CURSOR_TABLE *start, WT_CURSOR_TABLE *stop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_thread_group_create(WT_SESSION_IMPL *session, WT_THREAD_GROUP *group,
  const char *name, uint32_t min, uint32_t max, uint32_t flags,
  bool (*chk_func)(WT_SESSION_IMPL *session),
  int (*run_func)(WT_SESSION_IMPL *session, WT_THREAD *context),
  int (*stop_func)(WT_SESSION_IMPL *session, WT_THREAD *context))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_thread_group_destroy(WT_SESSION_IMPL *session, WT_THREAD_GROUP *group)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_thread_group_resize(WT_SESSION_IMPL *session, WT_THREAD_GROUP *group,
  uint32_t new_min, uint32_t new_max, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_bucket_config(WT_SESSION_IMPL *session, const char *cfg[],
  WT_BUCKET_STORAGE **bstoragep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_conn_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_discard(WT_SESSION_IMPL *session, WT_TIERED *tiered)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_name(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, uint32_t id,
  uint32_t flags, const char **retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_open(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_opener(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
  WT_BLOCK_FILE_OPENER **openerp, const char **filenamep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_put_drop_local(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_put_drop_shared(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_put_flush(WT_SESSION_IMPL *session, WT_TIERED *tiered)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_put_flush_finish(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_set_metadata(WT_SESSION_IMPL *session, WT_TIERED *tiered, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_storage_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_storage_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_switch(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_tree_close(WT_SESSION_IMPL *session, WT_TIERED_TREE *tiered_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_tree_create(WT_SESSION_IMPL *session, const char *uri, bool exclusive,
  bool import, const char *config) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_tree_open(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_time_aggregate_validate(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta,
  WT_TIME_AGGREGATE *parent, bool silent) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_time_value_validate(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw,
  WT_TIME_AGGREGATE *parent, bool silent) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_timing_stress_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk_count(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *walkcntp,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk_custom_skip(WT_SESSION_IMPL *session, WT_REF **refp,
  int (*skip_func)(WT_SESSION_IMPL *, WT_REF *, void *, bool *), void *func_cookie, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk_skip(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *skipleafcntp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_exists(WT_SESSION_IMPL *session, bool *existp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_init(WT_SESSION_IMPL *session, bool verify_meta)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_read(WT_SESSION_IMPL *session, const char *key, char **valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_update(WT_SESSION_IMPL *session, const char *key, const char *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_validate_version(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_activity_drain(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[], bool waiting)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_checkpoint_log(WT_SESSION_IMPL *session, bool full, uint32_t flags,
  WT_LSN *lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_checkpoint_logread(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_LSN *ckpt_lsn) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_get_pinned_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *tsp,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_global_init(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_global_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_global_shutdown(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_init(WT_SESSION_IMPL *session, WT_SESSION_IMPL *session_ret)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_is_blocking(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_log_commit(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_log_op(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_op_printlog(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_parse_timestamp(WT_SESSION_IMPL *session, const char *name,
  wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_parse_timestamp_raw(WT_SESSION_IMPL *session, const char *name,
  wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_prepare(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_printlog(WT_SESSION *wt_session, const char *ofile, uint32_t flags,
  WT_LSN *start_lsn, WT_LSN *end_lsn) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_query_timestamp(WT_SESSION_IMPL *session, char *hex_timestamp,
  const char *cfg[], bool global_txn) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_reconfigure(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_recover(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_rollback_required(WT_SESSION_IMPL *session, const char *reason)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_commit_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t commit_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_durable_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t durable_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_prepare_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t prepare_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_read_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t read_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_truncate_log(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *start,
  WT_CURSOR_BTREE *stop) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_ts_log(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_update_oldest(WT_SESSION_IMPL *session, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_update_pinned_timestamp(WT_SESSION_IMPL *session, bool force)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_unexpected_object_type(
  WT_SESSION_IMPL *session, const char *uri, const char *expect) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_update_vector_push(WT_UPDATE_VECTOR *updates, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_upgrade(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_value_return_buf(WT_CURSOR_BTREE *cbt, WT_REF *ref, WT_ITEM *buf,
  WT_TIME_WINDOW *tw) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_cache(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_handles(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_log(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_sessions(WT_SESSION_IMPL *session, bool show_cursors)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_txn(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_txn_one(WT_SESSION_IMPL *session, WT_SESSION_IMPL *txn_session,
  int error_code, const char *error_string) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_update(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify_ckpt_load(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify_ckpt_unload(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify_dsk(WT_SESSION_IMPL *session, const char *tag, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify_dsk_image(WT_SESSION_IMPL *session, const char *tag,
  const WT_PAGE_HEADER *dsk, size_t size, WT_ADDR *addr, bool empty_page_ok)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int64_t __wt_log_slot_release(WT_MYSLOT *myslot, int64_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern size_t __wt_json_unpack_char(u_char ch, u_char *buf, size_t bufsz, bool force_unicode)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern size_t __wt_json_unpack_str(u_char *dest, size_t dest_len, const u_char *src, size_t src_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern ssize_t __wt_json_strlen(const char *src, size_t srclen) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern u_int __wt_hazard_count(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_checksum_sw(const void *chunk, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_log2_int(uint32_t n) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_nlpo2(uint32_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_nlpo2_round(uint32_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_random(WT_RAND_STATE volatile *rnd_state) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_rduppo2(uint32_t n, uint32_t po2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_split_page_size(int split_pct, uint32_t maxpagesize, uint32_t allocsize)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_gen(WT_SESSION_IMPL *session, int which)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_hash_city64(const void *s, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_hash_fnv64(const void *string, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_session_gen(WT_SESSION_IMPL *session, int which)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_strtouq(const char *nptr, char **endptr, int base) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void *__wt_ext_scr_alloc(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, size_t size);
extern void __wt_abort(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn))
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_backup_destroy(WT_SESSION_IMPL *session);
extern void __wt_blkcache_get(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_BLKCACHE_ITEM **blkcache_retp, bool *foundp, bool *skip_cache_putp);
extern void __wt_blkcache_remove(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size);
extern void __wt_block_cache_destroy(WT_SESSION_IMPL *session);
extern void __wt_block_ckpt_destroy(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci);
extern void __wt_block_compact_get_progress_stats(WT_SESSION_IMPL *session, WT_BM *bm,
  uint64_t *pages_reviewedp, uint64_t *pages_skippedp, uint64_t *pages_rewrittenp);
extern void __wt_block_compact_progress(
  WT_SESSION_IMPL *session, WT_BLOCK *block, u_int *msg_countp);
extern void __wt_block_configure_first_fit(WT_BLOCK *block, bool on);
extern void __wt_block_ext_free(WT_SESSION_IMPL *session, WT_EXT *ext);
extern void __wt_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el);
extern void __wt_block_set_readonly(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((cold));
extern void __wt_block_size_free(WT_SESSION_IMPL *session, WT_SIZE *sz);
extern void __wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_DSRC_STATS *stats);
extern void __wt_bloom_hash(WT_BLOOM *bloom, WT_ITEM *key, WT_BLOOM_HASH *bhash);
extern void __wt_bloom_insert(WT_BLOOM *bloom, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_btcur_cache(WT_CURSOR_BTREE *cbt);
extern void __wt_btcur_init(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt);
extern void __wt_btcur_iterate_setup(WT_CURSOR_BTREE *cbt);
extern void __wt_btcur_open(WT_CURSOR_BTREE *cbt);
extern void __wt_btree_huffman_close(WT_SESSION_IMPL *session);
extern void __wt_cache_stats_update(WT_SESSION_IMPL *session);
extern void __wt_capacity_throttle(WT_SESSION_IMPL *session, uint64_t bytes, WT_THROTTLE_TYPE type);
extern void __wt_checkpoint_progress(WT_SESSION_IMPL *session, bool closing);
extern void __wt_checkpoint_signal(WT_SESSION_IMPL *session, wt_off_t logsize);
extern void __wt_checkpoint_tree_reconcile_update(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta);
extern void __wt_ckpt_verbose(WT_SESSION_IMPL *session, WT_BLOCK *block, const char *tag,
  const char *ckpt_name, const uint8_t *ckpt_string, size_t ckpt_size);
extern void __wt_cond_auto_wait(
  WT_SESSION_IMPL *session, WT_CONDVAR *cond, bool progress, bool (*run_func)(WT_SESSION_IMPL *));
extern void __wt_cond_auto_wait_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond, bool progress,
  bool (*run_func)(WT_SESSION_IMPL *), bool *signalled);
extern void __wt_config_init(WT_SESSION_IMPL *session, WT_CONFIG *conf, const char *str);
extern void __wt_config_initn(
  WT_SESSION_IMPL *session, WT_CONFIG *conf, const char *str, size_t len);
extern void __wt_config_subinit(WT_SESSION_IMPL *session, WT_CONFIG *conf, WT_CONFIG_ITEM *item);
extern void __wt_conn_config_discard(WT_SESSION_IMPL *session);
extern void __wt_conn_foc_discard(WT_SESSION_IMPL *session);
extern void __wt_conn_stat_init(WT_SESSION_IMPL *session);
extern void __wt_connection_destroy(WT_CONNECTION_IMPL *conn);
extern void __wt_curhs_clear_insert_success(WT_CURSOR *cursor);
extern void __wt_cursor_close(WT_CURSOR *cursor);
extern void __wt_cursor_get_hash(
  WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *to_dup, uint64_t *hash_value);
extern void __wt_cursor_key_order_reset(WT_CURSOR_BTREE *cbt);
extern void __wt_cursor_reopen(WT_CURSOR *cursor, WT_DATA_HANDLE *dhandle);
extern void __wt_cursor_set_key(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_key_notsup(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_notsup(WT_CURSOR *cursor);
extern void __wt_cursor_set_raw_key(WT_CURSOR *cursor, WT_ITEM *key);
extern void __wt_cursor_set_raw_value(WT_CURSOR *cursor, WT_ITEM *value);
extern void __wt_cursor_set_value(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_value_notsup(WT_CURSOR *cursor, ...);
extern void __wt_curstat_cache_walk(WT_SESSION_IMPL *session);
extern void __wt_curstat_dsrc_final(WT_CURSOR_STAT *cst);
extern void __wt_curtable_set_key(WT_CURSOR *cursor, ...);
extern void __wt_curtable_set_value(WT_CURSOR *cursor, ...);
extern void __wt_dhandle_update_write_gens(WT_SESSION_IMPL *session);
extern void __wt_encrypt_size(
  WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor, size_t incoming_size, size_t *sizep);
extern void __wt_err_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 6, 7)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_errx_func(WT_SESSION_IMPL *session, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 5, 6)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_event_handler_set(WT_SESSION_IMPL *session, WT_EVENT_HANDLER *handler);
extern void __wt_evict_file_exclusive_off(WT_SESSION_IMPL *session);
extern void __wt_evict_list_clear_page(WT_SESSION_IMPL *session, WT_REF *ref);
extern void __wt_evict_priority_clear(WT_SESSION_IMPL *session);
extern void __wt_evict_priority_set(WT_SESSION_IMPL *session, uint64_t v);
extern void __wt_evict_server_wake(WT_SESSION_IMPL *session);
extern void __wt_ext_scr_free(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, void *p);
extern void __wt_ext_spin_destroy(WT_EXTENSION_API *wt_api, WT_EXTENSION_SPINLOCK *ext_spinlock);
extern void __wt_ext_spin_lock(
  WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_EXTENSION_SPINLOCK *ext_spinlock);
extern void __wt_ext_spin_unlock(
  WT_EXTENSION_API *wt_api, WT_SESSION *session, WT_EXTENSION_SPINLOCK *ext_spinlock);
extern void __wt_fill_hex(
  const uint8_t *src, size_t src_max, uint8_t *dest, size_t dest_max, size_t *lenp);
extern void __wt_free_int(WT_SESSION_IMPL *session, const void *p_arg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_free_ref(WT_SESSION_IMPL *session, WT_REF *ref, int page_type, bool free_pages);
extern void __wt_free_ref_index(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_INDEX *pindex, bool free_pages);
extern void __wt_free_update_list(WT_SESSION_IMPL *session, WT_UPDATE **updp);
extern void __wt_gen_drain(WT_SESSION_IMPL *session, int which, uint64_t generation);
extern void __wt_gen_init(WT_SESSION_IMPL *session);
extern void __wt_gen_next(WT_SESSION_IMPL *session, int which, uint64_t *genp);
extern void __wt_gen_next_drain(WT_SESSION_IMPL *session, int which);
extern void __wt_hazard_close(WT_SESSION_IMPL *session);
extern void __wt_hs_close(WT_SESSION_IMPL *session);
extern void __wt_hs_upd_time_window(WT_CURSOR *hs_cursor, WT_TIME_WINDOW **twp);
extern void __wt_huffman_close(WT_SESSION_IMPL *session, void *huffman_arg);
extern void __wt_json_close(WT_SESSION_IMPL *session, WT_CURSOR *cursor);
extern void __wt_log_ckpt(WT_SESSION_IMPL *session, WT_LSN *ckpt_lsn);
extern void __wt_log_slot_activate(WT_SESSION_IMPL *session, WT_LOGSLOT *slot);
extern void __wt_log_slot_free(WT_SESSION_IMPL *session, WT_LOGSLOT *slot);
extern void __wt_log_slot_join(
  WT_SESSION_IMPL *session, uint64_t mysize, uint32_t flags, WT_MYSLOT *myslot);
extern void __wt_log_written_reset(WT_SESSION_IMPL *session);
extern void __wt_log_wrlsn(WT_SESSION_IMPL *session, int *yield);
extern void __wt_logmgr_compat_version(WT_SESSION_IMPL *session);
extern void __wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp);
extern void __wt_lsm_manager_clear_tree(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern void __wt_lsm_manager_free_work_unit(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT *entry);
extern void __wt_lsm_tree_readlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern void __wt_lsm_tree_readunlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern void __wt_lsm_tree_release(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern void __wt_lsm_tree_throttle(
  WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, bool decrease_only);
extern void __wt_lsm_tree_writelock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern void __wt_lsm_tree_writeunlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree);
extern void __wt_meta_checkpoint_free(WT_SESSION_IMPL *session, WT_CKPT *ckpt);
extern void __wt_meta_ckptlist_free(WT_SESSION_IMPL *session, WT_CKPT **ckptbasep);
extern void __wt_meta_saved_ckptlist_free(WT_SESSION_IMPL *session);
extern void __wt_meta_track_discard(WT_SESSION_IMPL *session);
extern void __wt_meta_track_sub_on(WT_SESSION_IMPL *session);
extern void __wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_optrack_flush_buffer(WT_SESSION_IMPL *s);
extern void __wt_optrack_record_funcid(
  WT_SESSION_IMPL *session, const char *func, uint16_t *func_idp);
extern void __wt_os_stdio(WT_SESSION_IMPL *session);
extern void __wt_ovfl_discard_free(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_ovfl_reuse_free(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep);
extern void __wt_print_huffman_code(void *huffman_arg, uint16_t symbol);
extern void __wt_random_init(WT_RAND_STATE volatile *rnd_state)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_random_init_seed(WT_SESSION_IMPL *session, WT_RAND_STATE volatile *rnd_state)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_read_row_time_window(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_TIME_WINDOW *tw);
extern void __wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_rec_col_fix_write_auxheader(WT_SESSION_IMPL *session, uint32_t entries,
  uint32_t aux_start_offset, uint32_t auxentries, uint8_t *image, size_t size);
extern void __wt_rec_dictionary_free(WT_SESSION_IMPL *session, WT_RECONCILE *r);
extern void __wt_rec_dictionary_reset(WT_RECONCILE *r);
extern void __wt_ref_addr_free(WT_SESSION_IMPL *session, WT_REF *ref);
extern void __wt_ref_out(WT_SESSION_IMPL *session, WT_REF *ref);
extern void __wt_root_ref_init(
  WT_SESSION_IMPL *session, WT_REF *root_ref, WT_PAGE *root, bool is_recno);
extern void __wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_schema_destroy_colgroup(WT_SESSION_IMPL *session, WT_COLGROUP **colgroupp);
extern void __wt_scr_discard(WT_SESSION_IMPL *session);
extern void __wt_session_close_cache(WT_SESSION_IMPL *session);
extern void __wt_session_gen_enter(WT_SESSION_IMPL *session, int which);
extern void __wt_session_gen_leave(WT_SESSION_IMPL *session, int which);
extern void __wt_stash_discard(WT_SESSION_IMPL *session);
extern void __wt_stash_discard_all(WT_SESSION_IMPL *session_safe, WT_SESSION_IMPL *session);
extern void __wt_stat_connection_aggregate(WT_CONNECTION_STATS **from, WT_CONNECTION_STATS *to);
extern void __wt_stat_connection_clear_all(WT_CONNECTION_STATS **stats);
extern void __wt_stat_connection_clear_single(WT_CONNECTION_STATS *stats);
extern void __wt_stat_connection_discard(WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *handle);
extern void __wt_stat_connection_init_single(WT_CONNECTION_STATS *stats);
extern void __wt_stat_dsrc_aggregate(WT_DSRC_STATS **from, WT_DSRC_STATS *to);
extern void __wt_stat_dsrc_aggregate_single(WT_DSRC_STATS *from, WT_DSRC_STATS *to);
extern void __wt_stat_dsrc_clear_all(WT_DSRC_STATS **stats);
extern void __wt_stat_dsrc_clear_single(WT_DSRC_STATS *stats);
extern void __wt_stat_dsrc_discard(WT_SESSION_IMPL *session, WT_DATA_HANDLE *handle);
extern void __wt_stat_dsrc_init_single(WT_DSRC_STATS *stats);
extern void __wt_stat_join_aggregate(WT_JOIN_STATS **from, WT_JOIN_STATS *to);
extern void __wt_stat_join_clear_all(WT_JOIN_STATS **stats);
extern void __wt_stat_join_clear_single(WT_JOIN_STATS *stats);
extern void __wt_stat_join_init_single(WT_JOIN_STATS *stats);
extern void __wt_stat_session_clear_single(WT_SESSION_STATS *stats);
extern void __wt_stat_session_init_single(WT_SESSION_STATS *stats);
extern void __wt_thread_group_start_one(
  WT_SESSION_IMPL *session, WT_THREAD_GROUP *group, bool is_locked);
extern void __wt_thread_group_stop_one(WT_SESSION_IMPL *session, WT_THREAD_GROUP *group);
extern void __wt_tiered_get_drop_local(
  WT_SESSION_IMPL *session, uint64_t now, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_get_drop_shared(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_get_flush(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_get_flush_finish(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_pop_work(
  WT_SESSION_IMPL *session, uint32_t type, uint64_t maxval, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_push_work(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT *entry);
extern void __wt_tiered_work_free(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT *entry);
extern void __wt_timestamp_to_hex_string(wt_timestamp_t ts, char *hex_timestamp);
extern void __wt_txn_bump_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_clear_durable_timestamp(WT_SESSION_IMPL *session);
extern void __wt_txn_clear_read_timestamp(WT_SESSION_IMPL *session);
extern void __wt_txn_destroy(WT_SESSION_IMPL *session);
extern void __wt_txn_get_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_global_destroy(WT_SESSION_IMPL *session);
extern void __wt_txn_op_free(WT_SESSION_IMPL *session, WT_TXN_OP *op);
extern void __wt_txn_publish_durable_timestamp(WT_SESSION_IMPL *session);
extern void __wt_txn_release(WT_SESSION_IMPL *session);
extern void __wt_txn_release_resources(WT_SESSION_IMPL *session);
extern void __wt_txn_release_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_stats_update(WT_SESSION_IMPL *session);
extern void __wt_txn_truncate_end(WT_SESSION_IMPL *session);
extern void __wt_update_vector_clear(WT_UPDATE_VECTOR *updates);
extern void __wt_update_vector_free(WT_UPDATE_VECTOR *updates);
extern void __wt_update_vector_init(WT_SESSION_IMPL *session, WT_UPDATE_VECTOR *updates);
extern void __wt_update_vector_peek(WT_UPDATE_VECTOR *updates, WT_UPDATE **updp);
extern void __wt_update_vector_pop(WT_UPDATE_VECTOR *updates, WT_UPDATE **updp);
extern void __wt_value_return(WT_CURSOR_BTREE *cbt, WT_UPDATE_VALUE *upd_value);
extern void __wt_verbose_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t ts, const char *msg);
extern void __wt_verbose_worker(WT_SESSION_IMPL *session, WT_VERBOSE_CATEGORY category,
  WT_VERBOSE_LEVEL level, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 4, 5)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold));
extern void __wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
static inline WT_BTREE *__wt_curhs_get_btree(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline WT_CELL *__wt_cell_leaf_value_parse(WT_PAGE *page, WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline WT_CURSOR_BTREE *__wt_curhs_get_cbt(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline WT_FILE_SYSTEM *__wt_fs_file_system(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline WT_IKEY *__wt_ref_key_instantiated(WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline WT_VISIBLE_TYPE __wt_txn_upd_visible_type(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_btree_dominating_cache(WT_SESSION_IMPL *session, WT_BTREE *btree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_btree_lsm_over_size(WT_SESSION_IMPL *session, uint64_t maxsize)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_btree_syncing_by_other_session(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_cache_aggressive(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_cache_full(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_cache_hs_dirty(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_cache_stuck(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_checksum_match(const void *chunk, size_t len, uint32_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_eviction_clean_needed(WT_SESSION_IMPL *session, double *pct_fullp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_eviction_dirty_needed(WT_SESSION_IMPL *session, double *pct_fullp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_eviction_needed(WT_SESSION_IMPL *session, bool busy, bool readonly,
  double *pct_fullp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_eviction_updates_needed(WT_SESSION_IMPL *session, double *pct_fullp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_failpoint(WT_SESSION_IMPL *session, uint64_t conn_flag, u_int probability)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_isalnum(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_isalpha(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_isascii(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_isdigit(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_isprint(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_isspace(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_leaf_page_can_split(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_off_page(WT_PAGE *page, const void *p)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_op_timer_fired(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_page_can_evict(WT_SESSION_IMPL *session, WT_REF *ref, bool *inmem_splitp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_page_del_active(WT_SESSION_IMPL *session, WT_REF *ref, bool visible_all)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_page_evict_clean(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_page_evict_retry(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_page_is_empty(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_page_is_modified(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_rec_need_split(WT_RECONCILE *r, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_ref_addr_copy(WT_SESSION_IMPL *session, WT_REF *ref, WT_ADDR_COPY *copy)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_ref_cas_state_int(WT_SESSION_IMPL *session, WT_REF *ref, uint8_t old_state,
  uint8_t new_state, const char *func, int line) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_ref_is_root(WT_REF *ref) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_row_leaf_value(WT_PAGE *page, WT_ROW *rip, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_row_leaf_value_is_encoded(WT_ROW *rip)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_session_can_wait(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_split_descent_race(WT_SESSION_IMPL *session, WT_REF *ref,
  WT_PAGE_INDEX *saved_pindex) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_tw_start_visible(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_tw_start_visible_all(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_tw_stop_visible(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_tw_stop_visible_all(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_upd_value_visible_all(WT_SESSION_IMPL *session,
  WT_UPDATE_VALUE *upd_value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_upd_visible(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_upd_visible_all(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_visible(WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_visible_all(WT_SESSION_IMPL *session, uint64_t id,
  wt_timestamp_t timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline bool __wt_txn_visible_id_snapshot(uint64_t id, uint64_t snap_min, uint64_t snap_max,
  uint64_t *snapshot, uint32_t snapshot_count) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline double __wt_eviction_dirty_target(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_btcur_skip_page(WT_SESSION_IMPL *session, WT_REF *ref, void *context,
  bool *skipp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_btree_block_free(WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_buf_extend(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_buf_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_buf_init(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_buf_initsize(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_buf_set(WT_SESSION_IMPL *session, WT_ITEM *buf, const void *data,
  size_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_buf_setstr(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *s)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_cache_eviction_check(WT_SESSION_IMPL *session, bool busy, bool readonly,
  bool *didworkp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_cell_pack_value_match(WT_CELL *page_cell, WT_CELL *val_cell,
  const uint8_t *val_data, bool *matchp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_cell_unpack_safe(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL *cell, WT_CELL_UNPACK_ADDR *unpack_addr, WT_CELL_UNPACK_KV *unpack_value, const void *end)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_check_addr_validity(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta,
  bool expected_error) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_col_append_serial(WT_SESSION_IMPL *session, WT_PAGE *page,
  WT_INSERT_HEAD *ins_head, WT_INSERT ***ins_stack, WT_INSERT **new_insp, size_t new_ins_size,
  uint64_t *recnop, u_int skipdepth, bool exclusive)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_compare(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
  const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_compare_skip(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
  const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp, size_t *matchp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_curindex_get_valuev(WT_CURSOR *cursor, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_cursor_func_init(WT_CURSOR_BTREE *cbt, bool reenter)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_cursor_localkey(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_curtable_get_valuev(WT_CURSOR *cursor, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_dsk_cell_data_ref(WT_SESSION_IMPL *session, int page_type, void *unpack_arg,
  WT_ITEM *store) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_extlist_read_pair(const uint8_t **p, wt_off_t *offp, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_extlist_write_pair(uint8_t **p, wt_off_t off, wt_off_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fclose(WT_SESSION_IMPL *session, WT_FSTREAM **fstrp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fextend(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fflush(WT_SESSION_IMPL *session, WT_FSTREAM *fstr)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_file_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fprintf(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_directory_list(
  WT_SESSION_IMPL *session, const char *dir, const char *prefix, char ***dirlistp, u_int *countp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_directory_list_free(WT_SESSION_IMPL *session, char ***dirlistp,
  u_int count) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_directory_list_single(
  WT_SESSION_IMPL *session, const char *dir, const char *prefix, char ***dirlistp, u_int *countp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_remove(WT_SESSION_IMPL *session, const char *name, bool durable)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_rename(WT_SESSION_IMPL *session, const char *from, const char *to,
  bool durable) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fs_size(WT_SESSION_IMPL *session, const char *name, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_getline(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_insert_serial(WT_SESSION_IMPL *session, WT_PAGE *page,
  WT_INSERT_HEAD *ins_head, WT_INSERT ***ins_stack, WT_INSERT **new_insp, size_t new_ins_size,
  u_int skipdepth, bool exclusive) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item, bool prefix)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_lex_compare_short(const WT_ITEM *user_item, const WT_ITEM *tree_item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_lex_compare_skip(const WT_ITEM *user_item, const WT_ITEM *tree_item,
  size_t *matchp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_log_cmp(WT_LSN *lsn1, WT_LSN *lsn2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_page_cell_data_ref(WT_SESSION_IMPL *session, WT_PAGE *page, void *unpack_arg,
  WT_ITEM *store) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_page_dirty_and_evict_soon(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_page_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_page_parent_modify_set(WT_SESSION_IMPL *session, WT_REF *ref, bool page_only)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_page_release(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_page_swap_func(
  WT_SESSION_IMPL *session, WT_REF *held, WT_REF *want, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_prefix_match(const WT_ITEM *prefix, const WT_ITEM *tree_item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_read(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len,
  void *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_rec_cell_build_val(WT_SESSION_IMPL *session, WT_RECONCILE *r,
  const void *data, size_t size, WT_TIME_WINDOW *tw, uint64_t rle)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_rec_dict_replace(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_TIME_WINDOW *tw, uint64_t rle, WT_REC_KV *val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_ref_block_free(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_row_leaf_key(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip,
  WT_ITEM *key, bool instantiate) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_row_leaf_key_instantiate(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_snprintf(char *buf, size_t size, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_snprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt,
  ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 4, 5)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_snprintf_len_set(char *buf, size_t size, size_t *retsizep, const char *fmt,
  ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 4, 5)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_spin_trylock_track(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_struct_packv(WT_SESSION_IMPL *session, void *buffer, size_t size,
  const char *fmt, va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_struct_sizev(WT_SESSION_IMPL *session, size_t *sizep, const char *fmt,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_struct_unpackv(WT_SESSION_IMPL *session, const void *buffer, size_t size,
  const char *fmt, va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_sync_and_rename(WT_SESSION_IMPL *session, WT_FSTREAM **fstrp,
  const char *from, const char *to) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_activity_check(WT_SESSION_IMPL *session, bool *txn_active)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_autocommit_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_begin(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_context_check(WT_SESSION_IMPL *session, bool requires_txn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_context_prepare_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_id_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_idle_cache_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_modify_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *upd, wt_timestamp_t *prev_tsp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_modify_page_delete(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_op_set_key(WT_SESSION_IMPL *session, const WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_read(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key,
  uint64_t recno, WT_UPDATE *upd) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_read_upd_list(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *upd) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_read_upd_list_internal(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *upd, WT_UPDATE **prepare_updp, WT_UPDATE **restored_updp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_txn_search_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_upd_alloc(WT_SESSION_IMPL *session, const WT_ITEM *value, u_int modify_type,
  WT_UPDATE **updp, size_t *sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_upd_alloc_tombstone(WT_SESSION_IMPL *session, WT_UPDATE **updp,
  size_t *sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_update_serial(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_PAGE *page,
  WT_UPDATE **srch_upd, WT_UPDATE **updp, size_t upd_size, bool exclusive)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vfprintf(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, const char *fmt,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vpack_int(uint8_t **pp, size_t maxlen, int64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vpack_negint(uint8_t **pp, size_t maxlen, uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vpack_posint(uint8_t **pp, size_t maxlen, uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vpack_uint(uint8_t **pp, size_t maxlen, uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vsnprintf_len_set(char *buf, size_t size, size_t *retsizep, const char *fmt,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vunpack_int(const uint8_t **pp, size_t maxlen, int64_t *xp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vunpack_negint(const uint8_t **pp, size_t maxlen, uint64_t *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vunpack_posint(const uint8_t **pp, size_t maxlen, uint64_t *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_vunpack_uint(const uint8_t **pp, size_t maxlen, uint64_t *xp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline int __wt_write(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len,
  const void *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_addr(WT_SESSION_IMPL *session, WT_CELL *cell, u_int cell_type,
  uint64_t recno, WT_TIME_AGGREGATE *ta, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_copy(WT_SESSION_IMPL *session, WT_CELL *cell,
  WT_TIME_WINDOW *tw, uint64_t rle, uint64_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_del(WT_SESSION_IMPL *session, WT_CELL *cell, WT_TIME_WINDOW *tw,
  uint64_t rle) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_int_key(WT_CELL *cell, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_leaf_key(WT_CELL *cell, uint8_t prefix, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_ovfl(WT_SESSION_IMPL *session, WT_CELL *cell, uint8_t type,
  WT_TIME_WINDOW *tw, uint64_t rle, size_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_pack_value(WT_SESSION_IMPL *session, WT_CELL *cell,
  WT_TIME_WINDOW *tw, uint64_t rle, size_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_cell_total_len(void *unpack_arg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_strnlen(const char *s, size_t maxlen)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_update_list_memsize(WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_vsize_int(int64_t x) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_vsize_negint(uint64_t x) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_vsize_posint(uint64_t x) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline size_t __wt_vsize_uint(uint64_t x) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline u_char __wt_hex(int c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline u_char __wt_tolower(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline u_int __wt_cell_type(WT_CELL *cell) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline u_int __wt_cell_type_raw(WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline u_int __wt_skip_choose_depth(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_btree_bytes_evictable(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_btree_bytes_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_btree_bytes_updates(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_btree_dirty_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_btree_dirty_leaf_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_bytes_image(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_bytes_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_bytes_other(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_bytes_plus_overhead(WT_CACHE *cache, uint64_t sz)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_bytes_updates(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_dirty_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_dirty_leaf_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_pages_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cache_read_gen(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_cell_rle(WT_CELL_UNPACK_KV *unpack)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_clock(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_clock_to_nsec(uint64_t end, uint64_t begin)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_rdtsc(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_safe_sub(uint64_t v1, uint64_t v2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_txn_id_alloc(WT_SESSION_IMPL *session, bool publish)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline uint64_t __wt_txn_oldest_id(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static inline void __wt_buf_free(WT_SESSION_IMPL *session, WT_ITEM *buf);
static inline void __wt_cache_decr_check_size(
  WT_SESSION_IMPL *session, size_t *vp, size_t v, const char *fld);
static inline void __wt_cache_decr_check_uint64(
  WT_SESSION_IMPL *session, uint64_t *vp, uint64_t v, const char *fld);
static inline void __wt_cache_dirty_decr(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cache_dirty_incr(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cache_page_byte_dirty_decr(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static inline void __wt_cache_page_byte_updates_decr(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static inline void __wt_cache_page_evict(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cache_page_image_decr(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cache_page_image_incr(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cache_page_inmem_decr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static inline void __wt_cache_page_inmem_incr(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static inline void __wt_cache_read_gen_bump(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cache_read_gen_incr(WT_SESSION_IMPL *session);
static inline void __wt_cache_read_gen_new(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_cell_type_reset(
  WT_SESSION_IMPL *session, WT_CELL *cell, u_int old_type, u_int new_type);
static inline void __wt_cell_unpack_addr(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL *cell, WT_CELL_UNPACK_ADDR *unpack_addr);
static inline void __wt_cell_unpack_kv(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL *cell, WT_CELL_UNPACK_KV *unpack_value);
static inline void __wt_cond_wait(
  WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs, bool (*run_func)(WT_SESSION_IMPL *));
static inline void __wt_cursor_dhandle_decr_use(WT_SESSION_IMPL *session);
static inline void __wt_cursor_dhandle_incr_use(WT_SESSION_IMPL *session);
static inline void __wt_cursor_disable_bulk(WT_SESSION_IMPL *session);
static inline void __wt_epoch(WT_SESSION_IMPL *session, struct timespec *tsp);
static inline void __wt_op_timer_start(WT_SESSION_IMPL *session);
static inline void __wt_op_timer_stop(WT_SESSION_IMPL *session);
static inline void __wt_page_evict_soon(WT_SESSION_IMPL *session, WT_REF *ref);
static inline void __wt_page_modify_clear(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_page_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_page_only_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page);
static inline void __wt_rec_auximage_copy(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t count, WT_REC_KV *kv);
static inline void __wt_rec_auxincr(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t v, size_t size);
static inline void __wt_rec_cell_build_addr(WT_SESSION_IMPL *session, WT_RECONCILE *r,
  WT_ADDR *addr, WT_CELL_UNPACK_ADDR *vpack, bool proxy_cell, uint64_t recno);
static inline void __wt_rec_image_copy(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_KV *kv);
static inline void __wt_rec_incr(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t v, size_t size);
static inline void __wt_rec_time_window_clear_obsolete(WT_SESSION_IMPL *session,
  WT_UPDATE_SELECT *upd_select, WT_CELL_UNPACK_KV *vpack, WT_RECONCILE *r);
static inline void __wt_ref_key(WT_PAGE *page, WT_REF *ref, void *keyp, size_t *sizep);
static inline void __wt_ref_key_clear(WT_REF *ref);
static inline void __wt_ref_key_onpage_set(WT_PAGE *page, WT_REF *ref, WT_CELL_UNPACK_ADDR *unpack);
static inline void __wt_row_leaf_key_free(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip);
static inline void __wt_row_leaf_key_info(WT_PAGE *page, void *copy, WT_IKEY **ikeyp,
  WT_CELL **cellp, void *datap, size_t *sizep, uint8_t *prefixp);
static inline void __wt_row_leaf_key_set(WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK_KV *unpack);
static inline void __wt_row_leaf_value_cell(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK_KV *vpack);
static inline void __wt_row_leaf_value_set(WT_ROW *rip, WT_CELL_UNPACK_KV *unpack);
static inline void __wt_scr_free(WT_SESSION_IMPL *session, WT_ITEM **bufp);
static inline void __wt_seconds(WT_SESSION_IMPL *session, uint64_t *secondsp);
static inline void __wt_seconds32(WT_SESSION_IMPL *session, uint32_t *secondsp);
static inline void __wt_spin_backoff(uint64_t *yield_count, uint64_t *sleep_usecs);
static inline void __wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static inline void __wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static inline void __wt_spin_lock_track(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static inline void __wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static inline void __wt_struct_size_adjust(WT_SESSION_IMPL *session, size_t *sizep);
static inline void __wt_timing_stress(WT_SESSION_IMPL *session, u_int flag);
static inline void __wt_tree_modify_set(WT_SESSION_IMPL *session);
static inline void __wt_txn_cursor_op(WT_SESSION_IMPL *session);
static inline void __wt_txn_err_set(WT_SESSION_IMPL *session, int ret);
static inline void __wt_txn_op_apply_prepare_state(
  WT_SESSION_IMPL *session, WT_REF *ref, bool commit);
static inline void __wt_txn_op_delete_commit_apply_timestamps(
  WT_SESSION_IMPL *session, WT_REF *ref);
static inline void __wt_txn_op_set_recno(WT_SESSION_IMPL *session, uint64_t recno);
static inline void __wt_txn_op_set_timestamp(WT_SESSION_IMPL *session, WT_TXN_OP *op);
static inline void __wt_txn_pinned_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *pinned_tsp);
static inline void __wt_txn_read_last(WT_SESSION_IMPL *session);
static inline void __wt_txn_timestamp_flags(WT_SESSION_IMPL *session);
static inline void __wt_txn_unmodify(WT_SESSION_IMPL *session);
static inline void __wt_upd_value_assign(WT_UPDATE_VALUE *upd_value, WT_UPDATE *upd);
static inline void __wt_upd_value_clear(WT_UPDATE_VALUE *upd_value);
