#pragma once

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern WT_DATA_SOURCE *__wt_schema_get_source(WT_SESSION_IMPL *session, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern WT_EXT *__wt_block_off_srch_inclusive(WT_EXTLIST *el, wt_off_t off)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern WT_HAZARD *__wt_hazard_check(WT_SESSION_IMPL *session, WT_REF *ref,
  WT_SESSION_IMPL **sessionp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_block_disagg_manager_owns_object(WT_SESSION_IMPL *session, const char *uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_block_extlist_can_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block,
  WT_EXTLIST *el) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_checksum_alt_match(const void *chunk, size_t len, uint32_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_compact_check_eligibility(WT_SESSION_IMPL *session, const char *uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_config_get_choice(const char **choices, WT_CONFIG_ITEM *item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_conn_is_disagg(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_fsync_background_chk(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_gen_active(WT_SESSION_IMPL *session, int which, uint64_t generation)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_handle_is_open(WT_SESSION_IMPL *session, const char *name, bool locked)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_hazard_check_assert(WT_SESSION_IMPL *session, void *ref, bool waitfor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_is_valid_sub_level_error(int sub_level_err)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_ispo2(uint32_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_modify_idempotent(const void *modify)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_read_cell_time_window(WT_CURSOR_BTREE *cbt, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_rwlock_islocked(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_session_prefetch_check(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wt_txn_active(WT_SESSION_IMPL *session, uint64_t txnid)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_block_disagg_is_mapped(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_block_offset_invalid(WT_BLOCK *block, wt_off_t offset, uint32_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_cell_type_check(uint8_t cell_type, uint8_t dsk_type)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_delete_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, bool visible_all)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_rts_visibility_has_stable_update(WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_rts_visibility_page_needs_abort(WT_SESSION_IMPL *session, WT_REF *ref,
  wt_timestamp_t rollback_timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_rts_visibility_txn_visible_id(WT_SESSION_IMPL *session, uint64_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern char *__wt_time_aggregate_to_string(WT_TIME_AGGREGATE *ta, char *ta_string)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern char *__wt_time_point_to_string(wt_timestamp_t durable_ts, wt_timestamp_t ts,
  wt_timestamp_t prepare_ts, uint64_t prepared_id, uint64_t txn_id, char *tp_string)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern const char *__wti_cell_type_string(uint8_t type)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern double __wt_page_npos(WT_SESSION_IMPL *session, WT_REF *ref, double start, char *path_str,
  size_t *path_str_offsetp, size_t path_str_sz_max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_apply_single_idx(WT_SESSION_IMPL *session, WT_INDEX *idx, WT_CURSOR *cur,
  WT_CURSOR_TABLE *ctable, int (*f)(WT_CURSOR *)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_background_compact_end(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_background_compact_signal(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_background_compact_start(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_file_remove(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_load_incr(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *blkcfg,
  WT_ITEM *bitstring, uint64_t nbits) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_backup_set_blkincr(WT_SESSION_IMPL *session, uint64_t i, uint64_t granularity,
  const char *id, uint64_t id_len) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bad_object_type(WT_SESSION_IMPL *session, const char *uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_get_handle(WT_SESSION_IMPL *session, WT_BM *bm, uint32_t objectid,
  bool reading, WT_BLOCK **blockp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  bool forced_salvage, bool readonly, uint32_t allocsize, WT_LIVE_RESTORE_FH_META *lr_fh_meta,
  WT_BM **bmp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_read(WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_read_multi(WT_SESSION_IMPL *session, WT_ITEM **buf, size_t *buf_count,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_blkcache_write(WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, uint8_t *addr, size_t *addr_sizep, size_t *compressed_sizep,
  bool checkpoint, bool checkpoint_io, bool compressed)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_invalid(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size, bool live) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_pack(WT_BLOCK *block, uint8_t **pp, uint32_t objectid, wt_off_t offset,
  uint32_t size, uint32_t checksum) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_string(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  const uint8_t *addr, size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_addr_unpack(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *p,
  size_t addr_size, uint32_t *objectidp, wt_off_t *offsetp, uint32_t *sizep, uint32_t *checksump)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_checkpoint(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  WT_CKPT *ckptbase, bool data_checksum) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_block_disagg_manager_create(WT_SESSION_IMPL *session, WT_BUCKET_STORAGE *bstorage,
  const char *filename) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_disagg_manager_open(WT_SESSION_IMPL *session, const char *uri,
  const char *cfg[], bool forced_salvage, bool readonly, WT_BM **bmp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_free(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_create(WT_SESSION_IMPL *session, const char *filename,
  uint32_t allocsize) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_drop(WT_SESSION_IMPL *session, const char *filename, bool durable)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_drop_object(WT_SESSION_IMPL *session, WT_BUCKET_STORAGE *bstorage,
  const char *filename, bool durable) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_named_size(WT_SESSION_IMPL *session, const char *name,
  wt_off_t *sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_manager_size(WT_BM *bm, WT_SESSION_IMPL *session, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_open(WT_SESSION_IMPL *session, const char *filename, uint32_t objectid,
  const char *cfg[], bool forced_salvage, bool readonly, bool fixed, uint32_t allocsize,
  WT_LIVE_RESTORE_FH_META *lr_fh_meta, WT_BLOCK **blockp)
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
extern int __wt_block_verify_addr(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_verify_end(WT_SESSION_IMPL *session, WT_BLOCK *block, bool verify_success)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_verify_start(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_CKPT *ckptbase,
  const char *cfg[]) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_block_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, uint8_t *addr, size_t *addr_sizep, bool data_checksum,
  bool checkpoint_io) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_bloom_open(WT_SESSION_IMPL *session, const char *uri, uint32_t factor, uint32_t k,
  WT_CURSOR *owner, WT_BLOOM **bloomp) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bm_corrupt(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bm_corrupt_dump(WT_SESSION_IMPL *session, WT_ITEM *buf, uint32_t objectid,
  wt_off_t offset, uint32_t size, uint32_t checksum) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bm_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bm_sweep_handles(WT_SESSION_IMPL *session, WT_BM *bm)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_btcur_next_random(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_prev(WT_CURSOR_BTREE *cbt, bool truncating)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btcur_range_truncate(WT_TRUNCATE_INFO *trunc_info)
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
extern int __wt_btree_open(WT_SESSION_IMPL *session, const char *op_cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_stat_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_btree_switch_object(WT_SESSION_IMPL *session, uint32_t objectid)
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
extern int __wt_cache_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_pool_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cache_pool_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_calc_modify(WT_SESSION_IMPL *wt_session, const WT_ITEM *oldv, const WT_ITEM *newv,
  size_t maxdiff, WT_MODIFY *entries, int *nentriesp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_begin_transaction(WT_SESSION_IMPL *session, const char *config,
  int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_close_session(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_commit_transaction(WT_SESSION_IMPL *session, const char *config,
  int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_open_session(WT_SESSION_IMPL *session, int ret_val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_prepare_transaction(WT_SESSION_IMPL *session, const char *config,
  int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_prepared_id_transaction(WT_SESSION_IMPL *session, const char *config,
  int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_prepared_id_transaction_uint(WT_SESSION_IMPL *session,
  uint64_t prepared_id, int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_print_return(WT_CONNECTION_IMPL *conn, WT_SESSION_IMPL *session,
  int ret_val, const char *err_msg) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_query_timestamp(
  WT_SESSION_IMPL *session, const char *config, const char *hex_timestamp, int ret_val, bool global)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_rollback_transaction(WT_SESSION_IMPL *session, const char *config,
  int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_set_timestamp(WT_SESSION_IMPL *session, const char *config, int ret_val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_timestamp_transaction(WT_SESSION_IMPL *session, const char *config,
  int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_call_log_timestamp_transaction_uint(WT_SESSION_IMPL *session, WT_TS_TXN_TYPE which,
  wt_timestamp_t ts, int ret_val) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_calloc(WT_SESSION_IMPL *session, size_t number, size_t size, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_cleanup_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_cleanup_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_checkpoint_log(WT_SESSION_IMPL *session, bool full, uint32_t flags, WT_LSN *lsnp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_create_from_metadata(WT_SESSION_IMPL *session, const char *name,
  uint32_t id, wt_off_t file_offset, uint64_t cache_offset, size_t chunk_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_free_external(
  WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t objectid, wt_off_t offset, uint32_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_get(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t objectid,
  wt_off_t offset, uint32_t size, void *dst, bool *cache_hit)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_ingest(WT_SESSION_IMPL *session, const char *local_name,
  const char *sp_obj_name, uint32_t objectid) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_salvage(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_setup(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_chunkcache_teardown(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_clayered_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_close(WT_SESSION_IMPL *session, WT_FH **fhp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_close_connection_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_col_modify(WT_CURSOR_BTREE *cbt, uint64_t recno, const WT_ITEM *value,
  WT_UPDATE **updp_arg, u_int modify_type, bool exclusive, bool restore)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_conf_bind(WT_SESSION_IMPL *session, const char *compiled_str, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conf_compile(WT_SESSION_IMPL *session, const char *api, const char *format,
  const char **resultp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conf_compile_api_call(WT_SESSION_IMPL *session, const WT_CONFIG_ENTRY *centry,
  u_int centry_index, const char *config, void *compile_buf, size_t compile_buf_size,
  WT_CONF **confp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conf_compile_init(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conf_gets_func(WT_SESSION_IMPL *session, const WT_CONF *orig_conf,
  uint64_t orig_keys, int def, bool use_def, WT_CONFIG_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_check(WT_SESSION_IMPL *session, const WT_CONFIG_ENTRY *entry,
  const char *config, size_t config_len) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_config_collapse(WT_SESSION_IMPL *session, const char **cfg, char **config_ret)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_config_tiered_strip(WT_SESSION_IMPL *session, const char **cfg,
  const char **config_ret) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_configure_method(WT_SESSION_IMPL *session, const char *method, const char *uri,
  const char *config, const char *type, const char *check)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_btree_apply(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_call_log_setup(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_call_log_teardown(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_config_init(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_alloc(WT_SESSION_IMPL *session, const char *uri,
  const char *checkpoint) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_close(WT_SESSION_IMPL *session, bool final, bool mark_dead,
  bool check_visibility) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_close_all(WT_SESSION_IMPL *session, const char *uri, bool removed,
  bool mark_dead, bool check_visibility) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_find(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_dhandle_open(WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_page_history_track_evict(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_page_history_track_read(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_prefetch_clear_tree(WT_SESSION_IMPL *session, bool all)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_conn_prefetch_queue_push(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_copy_and_sync(WT_SESSION *wt_session, const char *from, const char *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curbackup_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curconfig_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curds_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_DATA_SOURCE *dsrc, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curfile_insert_check(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curfile_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_cache(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_get_cached(WT_SESSION_IMPL *session, uint32_t hs_id, WT_BTREE **hs_btreep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_next_hs_id(WT_SESSION_IMPL *session, uint32_t hs_id, uint32_t *next_hs_idp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_open(WT_SESSION_IMPL *session, uint32_t btree_id, WT_CURSOR *owner,
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_open_ext(WT_SESSION_IMPL *session, uint32_t hs_id, uint32_t btree_id,
  WT_CURSOR *owner, WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_range_truncate(WT_TRUNCATE_INFO *trunc_info)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_search_near_after(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_search_near_before(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curhs_set_btree_id(WT_SESSION_IMPL *session, WT_CURSOR *cursor, uint32_t btree_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curindex_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curmetadata_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner,
  const char *cfg[], WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_bounds_restore(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
  WT_CURSOR_BOUNDS_STATE *bounds_state) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_bounds_save(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
  WT_CURSOR_BOUNDS_STATE *bounds_state) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_cache_get(WT_SESSION_IMPL *session, const char *uri, uint64_t hash_value,
  WT_CURSOR *to_dup, const char **cfg, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_cached(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_config_notsup(WT_CURSOR *cursor, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_copy_release_item(WT_CURSOR *cursor, WT_ITEM *item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_dup_position(WT_CURSOR *to_dup, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_equals(WT_CURSOR *cursor, WT_CURSOR *other, int *equalp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_key(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_raw_key(WT_CURSOR *cursor, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_raw_key_value(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_raw_value(WT_CURSOR *cursor, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_get_value(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_init(WT_CURSOR *cursor, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_key_order_init(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_kv_not_set(WT_CURSOR *cursor, bool key) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_modify_notsup(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_notsup(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_prepared_discover_open(WT_SESSION_IMPL *session, const char *uri,
  WT_CURSOR *other, const char *cfg[], WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_reopen_notsup(WT_CURSOR *cursor, bool check_only)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_search_near_notsup(WT_CURSOR *cursor, int *exact)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_cursor_truncate(WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop,
  int (*rmfunc)(WT_CURSOR_BTREE *, const WT_ITEM *, u_int))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_colgroup_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_index_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_curstat_table_init(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR_STAT *cst) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_debug_cursor_tree_hs(void *cursor_arg, const char *ofile)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_debug_offset(WT_SESSION_IMPL *session, wt_off_t offset, uint32_t size,
  uint32_t checksum, const char *ofile, bool dump_all_data, bool dump_key_data)
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
extern int __wt_delete_page_rollback(WT_SESSION_IMPL *session, WT_TXN_OP *op)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_delete_redo_window_cleanup(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_dhandle_update_write_gens(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_disagg_advance_checkpoint(WT_SESSION_IMPL *session, bool ckpt_success)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_disagg_copy_metadata_later(WT_SESSION_IMPL *session, const char *stable_uri,
  const char *table_name) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_disagg_copy_metadata_process(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_disagg_copy_shared_metadata_layered(WT_SESSION_IMPL *session, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_disagg_put_checkpoint_meta(WT_SESSION_IMPL *session, const char *checkpoint_root,
  size_t checkpoint_root_size, uint64_t checkpoint_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_disagg_update_shared_metadata(WT_SESSION_IMPL *session, const char *key,
  const char *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_encrypt(WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor, size_t skip,
  WT_ITEM *in, WT_ITEM *out) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_encryptor_config(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval,
  WT_CONFIG_ITEM *keyid, WT_CONFIG_ARG *cfg_arg, WT_KEYED_ENCRYPTOR **kencryptorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_errno(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_error_log_add(const char *file, const char *func, int line, const char *expr,
  int error, int suberror) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_esc_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
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
extern int __wt_file_zero(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t start_off, wt_off_t size,
  WT_THROTTLE_TYPE type) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_filename(WT_SESSION_IMPL *session, const char *name, char **path)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_filename_construct(WT_SESSION_IMPL *session, const char *path,
  const char *file_prefix, uintmax_t id_1, uint32_t id_2, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_find_import_metadata(WT_SESSION_IMPL *session, const char *uri, const char **config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_fopen(WT_SESSION_IMPL *session, const char *name, uint32_t open_flags,
  uint32_t flags, WT_FSTREAM **fstrp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_fsync_background(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_getopt(const char *progname, int nargc, char *const *nargv, const char *ostr)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hash_map_get(WT_SESSION_IMPL *session, WT_HASH_MAP *hash_map, const void *key,
  size_t key_size, void **datap, size_t *data_sizep, bool insert_if_not_found, bool keep_locked)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hash_map_init(WT_SESSION_IMPL *session, WT_HASH_MAP **hash_mapp, size_t hash_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hazard_clear(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hazard_set_func(WT_SESSION_IMPL *session, WT_REF *ref, bool *busyp
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hex_to_raw(WT_SESSION_IMPL *session, const char *from, WT_ITEM *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_btree_truncate(WT_SESSION_IMPL *session, uint32_t btree_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_config(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_find_upd(WT_SESSION_IMPL *session, uint32_t btree_id, WT_ITEM *key,
  const char *value_format, uint64_t recno, WT_UPDATE_VALUE *upd_value, WT_ITEM *base_value_buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_modify(WT_CURSOR_BTREE *hs_cbt, WT_UPDATE *hs_upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_open(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_verify(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_hs_verify_one(WT_SESSION_IMPL *session, uint32_t btree_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_import_repair(WT_SESSION_IMPL *session, const char *uri, char **configp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_inmem_unsupported_op(WT_SESSION_IMPL *session, const char *tag)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_alloc_unpack(WT_SESSION_IMPL *session, const void *buffer, size_t size,
  const char *fmt, WT_JSON *json, bool iskey, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_column_init(WT_CURSOR *cursor, const char *uri, const char *keyformat,
  const WT_CONFIG_ITEM *idxconf, const WT_CONFIG_ITEM *colconf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_strncpy(WT_SESSION *wt_session, char **pdst, size_t dstlen, const char *src,
  size_t srclen) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_to_item(WT_SESSION_IMPL *session, const char *jstr, const char *format,
  WT_JSON *json, bool iskey, WT_ITEM *item) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_json_token(WT_SESSION *wt_session, const char *src, int *toktype,
  const char **tokstart, size_t *toklen) WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_key_return(WT_CURSOR_BTREE *cbt) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_layered_table_manager_add_table(WT_SESSION_IMPL *session, uint32_t ingest_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_library_init(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
  WT_CKPT *ckpt, WT_LIVE_RESTORE_FH_META *lr_fh_meta)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_checkpoint_by_name(WT_SESSION_IMPL *session, const char *uri,
  const char *checkpoint, int64_t *orderp, uint64_t *timep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_checkpoint_clear(WT_SESSION_IMPL *session, const char *fname)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_checkpoint_last_name(
  WT_SESSION_IMPL *session, const char *fname, const char **namep, int64_t *orderp, uint64_t *timep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_get(WT_SESSION_IMPL *session, const char *fname, bool update,
  WT_CKPT **ckptbasep, size_t *allocated) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_get_from_config(WT_SESSION_IMPL *session, bool update,
  WT_CKPT **ckptbasep, size_t *allocatedp, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_set(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
  WT_CKPT *ckptbase, const char *ckptlsn_str) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_to_meta(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_ckptlist_update_config(WT_SESSION_IMPL *session, WT_CKPT *ckptbase,
  const char *oldcfg, char **newcfgp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_correct_base_write_gen(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_load_prior_state(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_read_checkpoint_oldest(WT_SESSION_IMPL *session, const char *ckpt_name,
  wt_timestamp_t *timestampp, uint64_t *ckpttime) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_read_checkpoint_snapshot(WT_SESSION_IMPL *session, const char *ckpt_name,
  uint64_t *snap_write_gen, uint64_t *snap_min, uint64_t *snap_max, uint64_t **snapshot,
  uint32_t *snapshot_count, uint64_t *ckpttime) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_read_checkpoint_timestamp(WT_SESSION_IMPL *session, const char *ckpt_name,
  wt_timestamp_t *timestampp, uint64_t *ckpttime) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_sysinfo_clear(WT_SESSION_IMPL *session, const char *name, size_t namelen)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_sysinfo_set(WT_SESSION_IMPL *session, const char *name, size_t namelen)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_checkpoint(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_drop(WT_SESSION_IMPL *session, const char *filename)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_drop_object(WT_SESSION_IMPL *session, WT_BUCKET_STORAGE *bstorage,
  const char *filename) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_fileop(WT_SESSION_IMPL *session, const char *olduri, const char *newuri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_handle_lock(WT_SESSION_IMPL *session, bool created)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_init(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_off(WT_SESSION_IMPL *session, bool need_sync, bool unroll)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_on(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_track_sub_off(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_meta_update_connection(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_metadata_btree_id_to_uri(WT_SESSION_IMPL *session, uint32_t btree_id, char **uri)
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
extern int __wt_modify_apply_api(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_apply_item(WT_SESSION_IMPL *session, const char *value_format,
  WT_ITEM *value, const void *modify) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_pack(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries, WT_ITEM **modifyp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_modify_reconstruct_from_upd_list(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *modify, WT_UPDATE_VALUE *upd_value, WT_OP_CONTEXT context)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_msg(WT_SESSION_IMPL *session, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((cold)) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 2, 3)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_multi_to_ref(WT_SESSION_IMPL *session, WT_REF *old_ref, WT_PAGE *page,
  WT_MULTI *multi, size_t multi_entries, WT_REF **refp, size_t *incrp, bool first, bool closing)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_name_check(WT_SESSION_IMPL *session, const char *str, size_t len, bool check_uri)
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
extern int __wt_ovfl_read(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_COMMON *unpack,
  WT_ITEM *store) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_remove(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_KV *unpack)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_alloc(WT_SESSION_IMPL *session, uint8_t type, uint32_t alloc_entries,
  bool alloc_refs, WT_PAGE **pagep, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_from_npos(WT_SESSION_IMPL *session, WT_REF **refp, double npos,
  uint32_t read_flags, uint32_t walk_flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_from_npos_for_eviction(WT_SESSION_IMPL *session, WT_REF **refp, double npos,
  uint32_t read_flags, uint32_t walk_flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_from_npos_for_read(WT_SESSION_IMPL *session, WT_REF **refp, double npos,
  uint32_t read_flags, uint32_t walk_flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_in_func(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_modify_alloc(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_page_release_evict(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_panic_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 6, 7)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")))
      WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_prefetch_page_in(WT_SESSION_IMPL *session, WT_PREFETCH_QUEUE_ENTRY *pe)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_prepared_discover_filter_apply_handles(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_prepared_discover_find_item(WT_SESSION_IMPL *session, uint64_t prepared_id,
  WT_PENDING_PREPARED_ITEM **prepared_item) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_prepared_discover_remove_item(WT_SESSION_IMPL *session, uint64_t prepared_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_progress(WT_SESSION_IMPL *session, const char *s, uint64_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_random_descent(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags,
  WT_RAND_STATE *rnd) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_range_truncate(WT_CURSOR *start, WT_CURSOR *stop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_raw_to_esc_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size,
  WT_ITEM *to) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_raw_to_hex(WT_SESSION_IMPL *session, const uint8_t *from, size_t size, WT_ITEM *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_read_metadata_file(WT_SESSION_IMPL *session, const char *file,
  int (*meta_entry_worker_func)(WT_SESSION_IMPL *, WT_ITEM *, WT_ITEM *, void *), void *state,
  bool *file_exist) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_realloc(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret,
  size_t bytes_to_allocate, void *retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_realloc_noclear(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret,
  size_t bytes_to_allocate, void *retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_remove_if_exists(WT_SESSION_IMPL *session, const char *name, bool durable)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_remove_locked(WT_SESSION_IMPL *session, const char *name, bool *removed)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_reset_blkmod(WT_SESSION_IMPL *session, const char *orig_config, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rollback_to_stable_init(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rollback_to_stable_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_ikey_alloc(WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key,
  size_t size, WT_IKEY **ikeyp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_leaf_key_copy(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip,
  WT_ITEM *key) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_leaf_key_work(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip_arg,
  WT_ITEM *keyb, bool instantiate) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_modify(WT_CURSOR_BTREE *cbt, const WT_ITEM *key, const WT_ITEM *value,
  WT_UPDATE **updp_arg, u_int modify_type, bool exclusive, bool restore)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_row_search(WT_CURSOR_BTREE *cbt, WT_ITEM *srch_key, bool insert, WT_REF *leaf,
  bool leaf_safe, bool *leaf_foundp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_rwlock_init(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_salvage(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_alter(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_close_table(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  bool check_visibility) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_colgroup(WT_SESSION_IMPL *session, const char *uri, bool quiet,
  WT_TABLE **tablep, WT_COLGROUP **colgroupp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_table(WT_SESSION_IMPL *session, const char *name, size_t namelen,
  bool ok_incomplete, uint32_t flags, WT_TABLE **tablep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_get_table_uri(WT_SESSION_IMPL *session, const char *uri, bool ok_incomplete,
  uint32_t flags, WT_TABLE **tablep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_index(WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname,
  size_t len, WT_INDEX **indexp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_indices(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_layered(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_page_log(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name,
  WT_NAMED_PAGE_LOG **npage_logp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_open_storage_source(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name,
  WT_NAMED_STORAGE_SOURCE **nstoragep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_schema_range_truncate(WT_TRUNCATE_INFO *trunc_info)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_release_table(WT_SESSION_IMPL *session, WT_TABLE **tablep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_schema_tiered_shared_colgroup_name(WT_SESSION_IMPL *session, const char *tablename,
  bool active, WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_session_array_walk(WT_SESSION_IMPL *session,
  int (*walk_func)(WT_SESSION_IMPL *, WT_SESSION_IMPL *, bool *exit_walkp, void *cookiep),
  bool skip_internal, void *cookiep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_breakpoint(WT_SESSION *wt_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_close_internal(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_compact_check_interrupted(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_copy_values(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_create(WT_SESSION_IMPL *session, const char *uri, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_cursor_cache_sweep(WT_SESSION_IMPL *session, bool big_sweep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_dhandle_try_writelock(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_dump(WT_SESSION_IMPL *session, WT_SESSION_IMPL *dump_session,
  bool show_cursors) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_get_btree_ckpt(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  uint32_t flags, WT_DATA_HANDLE **hs_dhandlep, WT_CKPT_SNAPSHOT *ckpt_snapshot)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_get_dhandle(WT_SESSION_IMPL *session, const char *uri,
  const char *checkpoint, const char *cfg[], uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_lock_checkpoint(WT_SESSION_IMPL *session, const char *checkpoint)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_lock_dhandle(WT_SESSION_IMPL *session, uint32_t flags, bool *is_deadp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_range_truncate(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *start,
  WT_CURSOR *stop) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_release_dhandle(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_release_dhandle_v2(WT_SESSION_IMPL *session, bool check_visibility)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_release_resources(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_session_reset_cursors(WT_SESSION_IMPL *session, bool free_buffers)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_set_return_func(WT_SESSION_IMPL *session, const char *func, int line, int err,
  const char *strerr) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_insert(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_multi(WT_SESSION_IMPL *session, WT_REF *ref, int closing)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_reverse(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_split_rewrite(WT_SESSION_IMPL *session, WT_REF *ref, WT_MULTI *multi,
  bool change_ref_state) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_stat_session_desc(WT_CURSOR_STAT *cst, int slot, const char **p)
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
extern int __wt_struct_unpack(WT_SESSION_IMPL *session, const void *buffer, size_t len,
  const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_sync_file(WT_SESSION_IMPL *session, WT_CACHE_OP syncop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_table_range_truncate(WT_TRUNCATE_INFO *trunc_info)
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
extern int __wt_tiered_close(WT_SESSION_IMPL *session, WT_TIERED *tiered, bool final)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_conn_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_discard(WT_SESSION_IMPL *session, WT_TIERED *tiered, bool final)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_name(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, uint32_t id,
  uint32_t flags, const char **retp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_open(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_put_flush_finish(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_put_remove_local(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_set_metadata(WT_SESSION_IMPL *session, WT_TIERED *tiered, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_switch(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_tree_close(WT_SESSION_IMPL *session, WT_TIERED_TREE *tiered_tree)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tiered_tree_open(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_time_aggregate_validate(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta,
  WT_TIME_AGGREGATE *parent, bool silent) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_time_value_validate(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw,
  WT_TIME_AGGREGATE *parent, bool silent) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk_count(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *walkcntp,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_tree_walk_custom_skip(WT_SESSION_IMPL *session, WT_REF **refp,
  int (*skip_func)(WT_SESSION_IMPL *, WT_REF *, void *, bool, bool *), void *func_cookie,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_exists(WT_SESSION_IMPL *session, bool *existp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_init(WT_SESSION_IMPL *session, bool verify_meta, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_read(WT_SESSION_IMPL *session, const char *key, char **valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_update(WT_SESSION_IMPL *session, const char *key, const char *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_turtle_validate_version(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_activity_drain(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_config(WT_SESSION_IMPL *session, WT_CONF *conf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_global_init(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_global_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_global_shutdown(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_init(WT_SESSION_IMPL *session, WT_SESSION_IMPL *session_ret)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_init_checkpoint_cursor(WT_SESSION_IMPL *session, WT_CKPT_SNAPSHOT *snapinfo,
  WT_TXN **txn_ret) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_is_blocking(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_log_op(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_parse_prepared_id(WT_SESSION_IMPL *session, uint64_t *prepared_id,
  WT_CONFIG_ITEM *cval) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
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
extern int __wt_txn_recover(WT_SESSION_IMPL *session, const char *cfg[], bool disagg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[], bool api_call)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_prepared_id(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_prepared_id_uint(WT_SESSION_IMPL *session, uint64_t prepared_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[], bool commit)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_set_timestamp_uint(WT_SESSION_IMPL *session, WT_TS_TXN_TYPE which,
  wt_timestamp_t ts) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_snapshot_save_and_refresh(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_truncate_log(WT_TRUNCATE_INFO *trunc_info)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_update_oldest(WT_SESSION_IMPL *session, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_unexpected_object_type(
  WT_SESSION_IMPL *session, const char *uri, const char *expect) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_update_vector_push(WT_UPDATE_VECTOR *updates, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_value_return_buf(WT_CURSOR_BTREE *cbt, WT_REF *ref, WT_ITEM *buf,
  WT_TIME_WINDOW *tw) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_backup(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_metadata(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_sessions(WT_SESSION_IMPL *session, bool show_cursors)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_txn(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_txn_one(WT_SESSION_IMPL *session, WT_SESSION_IMPL *txn_session,
  int error_code, const char *error_string) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify_dsk(WT_SESSION_IMPL *session, const char *tag, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verify_dsk_image(WT_SESSION_IMPL *session, const char *tag,
  const WT_PAGE_HEADER *dsk, size_t size, WT_ADDR *addr, uint32_t verify_flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_background_compact_server_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_background_compact_server_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_blkcache_map(WT_SESSION_IMPL *session, WT_BLOCK *block, void **mapped_regionp,
  size_t *lengthp, void **mapped_cookiep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_blkcache_map_read(WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr,
  size_t addr_size, bool *foundp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_blkcache_put(WT_SESSION_IMPL *session, WT_ITEM *data, WT_ITEM *deltas,
  uint32_t num_deltas, WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size,
  bool write) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_blkcache_tiered_open(WT_SESSION_IMPL *session, const char *uri, uint32_t objectid,
  WT_BLOCK **blockp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_blkcache_unmap(WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapped_region,
  size_t length, void *mapped_cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_alloc(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t *offp,
  wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_checkpoint_extlist_dump(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_checkpoint_final(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  uint8_t **file_sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_ckpt_init(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_ckpt_pack(WT_SESSION_IMPL *session, WT_BLOCK *block, uint8_t **pp,
  WT_BLOCK_CKPT *ci, bool skip_avail) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_ckpt_unpack(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *ckpt,
  size_t ckpt_size, WT_BLOCK_CKPT *ci) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_addr_invalid(WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_addr_pack(WT_SESSION_IMPL *session, uint8_t **pp,
  const WT_BLOCK_DISAGG_ADDRESS_COOKIE *cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_addr_string(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf,
  const uint8_t *addr, size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_addr_unpack(WT_SESSION_IMPL *session, const uint8_t **buf,
  size_t buf_size, WT_BLOCK_DISAGG_ADDRESS_COOKIE *cookie)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_checkpoint(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *root_image,
  WT_PAGE_BLOCK_META *block_meta, WT_CKPT *ckptbase, bool data_checksum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_checkpoint_load(WT_BM *bm, WT_SESSION_IMPL *session,
  const uint8_t *addr, size_t addr_size, uint8_t *root_addr, size_t *root_addr_sizep,
  bool checkpoint) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_checkpoint_resolve(WT_BM *bm, WT_SESSION_IMPL *session, bool failed)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_checkpoint_start(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_checkpoint_unload(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_ckpt_pack(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg,
  uint8_t **buf, const WT_BLOCK_DISAGG_ADDRESS_COOKIE *root_cookie)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_ckpt_unpack(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg,
  const uint8_t *buf, size_t buf_size, WT_BLOCK_DISAGG_ADDRESS_COOKIE *root_cookie)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_close(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_compact_end(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_compact_page_skip(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_compact_skip(WT_BM *bm, WT_SESSION_IMPL *session, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_compact_start(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_corrupt(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_manager_size(WT_BM *bm, WT_SESSION_IMPL *session, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_map_discard(WT_BM *bm, WT_SESSION_IMPL *session, void *map,
  size_t len) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_open(WT_SESSION_IMPL *session, const char *filename,
  const char *cfg[], bool forced_salvage, bool readonly, WT_BLOCK **blockp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_page_discard(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg,
  const uint8_t *addr, size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_read_multiple(WT_BM *bm, WT_SESSION_IMPL *session,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size, WT_ITEM *buffer_array,
  u_int *buffer_count) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_salvage_end(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_salvage_next(WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr,
  size_t *addr_sizep, bool *eofp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_salvage_start(WT_BM *bm, WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_salvage_valid(WT_BM *bm, WT_SESSION_IMPL *session, uint8_t *addr,
  size_t addr_size, bool valid) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_sync(WT_BM *bm, WT_SESSION_IMPL *session, bool block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_verify_addr(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_verify_end(WT_BM *bm, WT_SESSION_IMPL *session, bool verify_success)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_verify_start(WT_BM *bm, WT_SESSION_IMPL *session, WT_CKPT *ckptbase,
  const char *cfg[]) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, uint8_t *addr, size_t *addr_sizep, bool data_checksum,
  bool checkpoint_io) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_write_internal(WT_SESSION_IMPL *session,
  WT_BLOCK_DISAGG *block_disagg, WT_ITEM *buf, WT_PAGE_BLOCK_META *block_meta, uint32_t *sizep,
  uint32_t *checksump, bool data_checksum, bool checkpoint_io)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_disagg_write_size(size_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_discard(WT_SESSION_IMPL *session, WT_BLOCK *block, size_t added_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_ext_alloc(WT_SESSION_IMPL *session, WT_EXT **extp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_ext_discard(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_ext_prealloc(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_check(WT_SESSION_IMPL *session, WT_EXTLIST *al, WT_EXTLIST *bl)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_dump(WT_SESSION_IMPL *session, WT_EXTLIST *el)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_init(WT_SESSION_IMPL *session, WT_EXTLIST *el, const char *name,
  const char *extname, bool track_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_merge(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *a,
  WT_EXTLIST *b) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_read(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t ckpt_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_read_avail(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t ckpt_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_extlist_write(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  WT_EXTLIST *additional) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_insert_ext(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t off, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_misplaced(WT_SESSION_IMPL *session, WT_BLOCK *block, const char *list,
  wt_off_t offset, uint32_t size, bool live, const char *func, int line)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_off_free(WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t objectid,
  wt_off_t offset, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_off_remove_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t off, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_read_off(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  uint32_t objectid, wt_off_t offset, uint32_t size, uint32_t checksum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_block_write_off(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf,
  wt_off_t *offsetp, uint32_t *sizep, uint32_t *checksump, bool data_checksum, bool checkpoint_io,
  bool caller_locked) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_bm_close_block(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_btcur_bounds_position(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next,
  bool *need_walkp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_btcur_evict_reposition(WT_CURSOR_BTREE *cbt)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_btree_new_leaf_page(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_btree_prefetch(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_btree_tree_open(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cache_eviction_controls_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_capacity_server_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_capacity_server_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_chunkcache_metadata_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_chunkcache_metadata_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_col_fix_read_auxheader(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_COL_FIX_AUXILIARY_HEADER *auxhdr) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_config_get(WT_SESSION_IMPL *session, const char **cfg_arg, WT_CONFIG_ITEM *key,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_compat_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_dhandle_discard(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_dhandle_discard_single(WT_SESSION_IMPL *session, bool final, bool mark_dead)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_dhandle_outdated(WT_SESSION_IMPL *session, const char *uri)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_optrack_setup(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_optrack_teardown(WT_SESSION_IMPL *session, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_page_history_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_page_history_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_remove_collator(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_remove_compressor(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_remove_data_source(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_remove_encryptor(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_remove_page_log(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_remove_storage_source(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_conn_statistics_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_connection_close(WT_CONNECTION_IMPL *conn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_connection_init(WT_CONNECTION_IMPL *conn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_connection_open(WT_CONNECTION_IMPL *conn, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_connection_workers(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curbackup_free_incr(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curbackup_open_incr(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  WT_CURSOR *cursor, const char *cfg[], WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curbulk_close(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curbulk_init(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool bitmap,
  bool skip_sort_check) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curdump_create(WT_CURSOR *child, WT_CURSOR *owner, WT_CURSOR **cursorp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curfile_next_random(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_curfile_search_near(WT_CURSOR *cursor, int *exact)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_bound(WT_CURSOR *cursor, WT_CONF *conf, WT_COLLATOR *collator)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_cache(WT_CURSOR *cursor, WT_DATA_HANDLE *dhandle)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_cache_release(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool *released)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_compare_notsup(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_equals_notsup(WT_CURSOR *cursor, WT_CURSOR *other, int *equalp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_get_keyv(WT_CURSOR *cursor, uint64_t flags, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_get_raw_key_value_notsup(WT_CURSOR *cursor, WT_ITEM *key, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_get_value_notsup(WT_CURSOR *cursor, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_get_valuev(WT_CURSOR *cursor, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_key_order_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, bool next)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_modify_value_format_notsup(WT_CURSOR *cursor, WT_MODIFY *entries,
  int nentries) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_noop(WT_CURSOR *cursor) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_reconfigure(WT_CURSOR *cursor, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_set_keyv(WT_CURSOR *cursor, uint64_t flags, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_set_valuev(WT_CURSOR *cursor, const char *fmt, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_cursor_valid(WT_CURSOR_BTREE *cbt, bool *valid, bool check_bounds)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_debug_disk(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, const char *ofile,
  bool dump_all_data, bool dump_key_data) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_debug_mode_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_debug_offset_blind(WT_SESSION_IMPL *session, wt_off_t offset, const char *ofile,
  bool dump_all_data, bool dump_key_data) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_debug_page(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile,
  bool dump_all_data, bool dump_key_data) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_delete_page(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_delete_page_instantiate(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_desc_write(WT_SESSION_IMPL *session, WT_FH *fh, uint32_t allocsize)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_disagg_conn_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_disagg_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_disagg_set_last_materialized_lsn(WT_SESSION_IMPL *session, uint64_t lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_ensure_clean_startup_dir(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_execute_handle_operation(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[], uint32_t open_flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_extra_diagnostics_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_heuristic_controls_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_hex2byte(const u_char *from, u_char *to)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_json_config(WT_SESSION_IMPL *session, const char *cfg[], bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_layered_table_manager_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_meta_track_insert(WT_SESSION_IMPL *session, const char *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_meta_track_update(WT_SESSION_IMPL *session, const char *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image,
  uint32_t flags, WT_PAGE **pagep, bool *instantiate_updp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_page_inmem_updates(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_page_reconstruct_deltas(WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *deltas,
  size_t delta_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_prefetch_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_prefetch_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_prepared_discover_add_artifact_ondisk_row(
  WT_SESSION_IMPL *session, uint64_t prepared_id, WT_TIME_WINDOW *tw, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_prepared_discover_add_artifact_upd(WT_SESSION_IMPL *session, uint64_t prepared_id,
  WT_ITEM *key, WT_UPDATE *upd) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_row_ikey(WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key,
  size_t size, WT_REF *ref) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_row_ikey_incr(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t cell_offset,
  const void *key, size_t size, WT_REF *ref) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_btree_abort_updates(WT_SESSION_IMPL *session, WT_REF *ref,
  wt_timestamp_t rollback_timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_btree_apply_all(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_btree_walk_btree(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_btree_walk_btree_apply(
  WT_SESSION_IMPL *session, const char *uri, const char *config, wt_timestamp_t rollback_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_btree_work_unit(WT_SESSION_IMPL *session, WT_RTS_WORK_UNIT *entry)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_history_btree_hs_truncate(WT_SESSION_IMPL *session, uint32_t btree_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_history_delete_hs(WT_SESSION_IMPL *session, WT_ITEM *key, wt_timestamp_t ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_rts_history_final_pass(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_backup_check(WT_SESSION_IMPL *session, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_colcheck(WT_SESSION_IMPL *session, const char *key_format,
  const char *value_format, WT_CONFIG_ITEM *colconf, u_int *kcolsp, u_int *vcolsp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_destroy_index(WT_SESSION_IMPL *session, WT_INDEX **idxp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_get_index(WT_SESSION_IMPL *session, const char *uri, bool invalidate,
  bool quiet, WT_INDEX **indexp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_get_tiered_uri(WT_SESSION_IMPL *session, const char *uri, uint32_t flags,
  WT_TIERED **tieredp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_internal_session(WT_SESSION_IMPL *session, WT_SESSION_IMPL **int_sessionp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_open_colgroups(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_release_table_gen(WT_SESSION_IMPL *session, WT_TABLE **tablep,
  bool check_visibility) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_release_tiered(WT_SESSION_IMPL *session, WT_TIERED **tieredp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_schema_session_release(WT_SESSION_IMPL *session, WT_SESSION_IMPL *int_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_session_compact(WT_SESSION *wt_session, const char *uri, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_session_compact_readonly(WT_SESSION *wt_session, const char *uri,
  const char *config) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_session_notsup(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_statlog_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_statlog_destroy(WT_SESSION_IMPL *session, bool is_close)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_struct_truncate(WT_SESSION_IMPL *session, const char *input_fmt, u_int ncols,
  WT_ITEM *format) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_sweep_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_sweep_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_sweep_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_table_check(WT_SESSION_IMPL *session, WT_TABLE *table)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_tiered_bucket_config(WT_SESSION_IMPL *session, const char *cfg[],
  WT_BUCKET_STORAGE **bstoragep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_tiered_put_flush(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id,
  uint64_t generation) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_tiered_put_remove_shared(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_tiered_storage_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_tiered_storage_destroy(WT_SESSION_IMPL *session, bool final_flush)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_timing_stress_config(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_tree_walk_skip(WT_SESSION_IMPL *session, WT_REF **refp, uint64_t *skipleafcntp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_txn_checkpoint_logread(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_LSN *ckpt_lsn) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_txn_log_commit(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_txn_set_read_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t read_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_txn_ts_log(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_verbose_dump_handles(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_verify_ckpt_load(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_verify_ckpt_unload(WT_SESSION_IMPL *session, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern size_t __wt_json_unpack_str(u_char *dest, size_t dest_len, const u_char *src, size_t src_len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern ssize_t __wt_json_strlen(const char *src, size_t srclen) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern u_int __wt_hazard_count(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_checksum_sw(const void *chunk, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_checksum_with_seed_sw(uint32_t seed, const void *chunk, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_curhs_get_btree_id(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_log2_int(uint32_t n) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_nlpo2(uint32_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_nlpo2_round(uint32_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_random(WT_RAND_STATE *rnd_state) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_rduppo2(uint32_t n, uint32_t po2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_cursor_checkpoint_id(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_hash_city64(const void *s, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_hash_fnv64(const void *string, size_t len)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint64_t __wt_strtouq(const char *nptr, char **endptr, int base) WT_GCC_FUNC_DECL_ATTRIBUTE(
  (visibility("default"))) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void *__wt_ext_scr_alloc(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, size_t size);
extern void __wt_abort(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn))
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_backup_destroy(WT_SESSION_IMPL *session);
extern void __wt_blkcache_destroy(WT_SESSION_IMPL *session);
extern void __wt_blkcache_release_handle(
  WT_SESSION_IMPL *session, WT_BLOCK *block, bool *last_release);
extern void __wt_blkcache_remove(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size);
extern void __wt_block_compact_get_progress_stats(
  WT_SESSION_IMPL *session, WT_BM *bm, uint64_t *pages_reviewedp);
extern void __wt_block_compact_progress(WT_SESSION_IMPL *session, WT_BLOCK *block);
extern void __wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_DSRC_STATS *stats);
extern void __wt_bloom_hash(WT_BLOOM *bloom, WT_ITEM *key, WT_BLOOM_HASH *bhash);
extern void __wt_bloom_insert(WT_BLOOM *bloom, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_bm_set_readonly(WT_SESSION_IMPL *session) WT_GCC_FUNC_DECL_ATTRIBUTE((cold));
extern void __wt_btcur_free_cached_memory(WT_CURSOR_BTREE *cbt);
extern void __wt_btcur_init(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt);
extern void __wt_btcur_open(WT_CURSOR_BTREE *cbt);
extern void __wt_cache_stats_update(WT_SESSION_IMPL *session);
extern void __wt_capacity_throttle(WT_SESSION_IMPL *session, uint64_t bytes, WT_THROTTLE_TYPE type);
extern void __wt_checkpoint_cleanup_trigger(WT_SESSION_IMPL *session);
extern void __wt_cond_auto_wait(
  WT_SESSION_IMPL *session, WT_CONDVAR *cond, bool progress, bool (*run_func)(WT_SESSION_IMPL *));
extern void __wt_cond_auto_wait_signal(WT_SESSION_IMPL *session, WT_CONDVAR *cond, bool progress,
  bool (*run_func)(WT_SESSION_IMPL *), bool *signalled);
extern void __wt_conf_compile_discard(WT_SESSION_IMPL *session);
extern void __wt_config_init(WT_SESSION_IMPL *session, WT_CONFIG *conf, const char *str);
extern void __wt_config_initn(
  WT_SESSION_IMPL *session, WT_CONFIG *conf, const char *str, size_t len);
extern void __wt_config_subinit(WT_SESSION_IMPL *session, WT_CONFIG *conf, WT_CONFIG_ITEM *item);
extern void __wt_conn_config_discard(WT_SESSION_IMPL *session);
extern void __wt_conn_foc_discard(WT_SESSION_IMPL *session);
extern void __wt_conn_stat_init(WT_SESSION_IMPL *session);
extern void __wt_cursor_close(WT_CURSOR *cursor);
extern void __wt_cursor_get_hash(
  WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *to_dup, uint64_t *hash_value);
extern void __wt_cursor_key_order_reset(WT_CURSOR_BTREE *cbt);
extern void __wt_cursor_set_key(WT_CURSOR *cursor, ...);
extern void __wt_cursor_set_raw_key(WT_CURSOR *cursor, WT_ITEM *key);
extern void __wt_cursor_set_raw_value(WT_CURSOR *cursor, WT_ITEM *value);
extern void __wt_cursor_set_value(WT_CURSOR *cursor, ...);
extern void __wt_curstat_dsrc_final(WT_CURSOR_STAT *cst);
extern void __wt_debug_crash(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_encrypt_size(
  WT_SESSION_IMPL *session, WT_KEYED_ENCRYPTOR *kencryptor, size_t incoming_size, size_t *sizep);
extern void __wt_err_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 6, 7)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_error_log_clear(void);
extern void __wt_error_log_to_handler(WT_SESSION_IMPL *session);
extern void __wt_errx_func(WT_SESSION_IMPL *session, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((cold))
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 5, 6)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_event_handler_set(WT_SESSION_IMPL *session, WT_EVENT_HANDLER *handler);
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
extern void __wt_free_obsolete_updates(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *visible_all_upd);
extern void __wt_free_update_list(WT_SESSION_IMPL *session, WT_UPDATE **updp);
extern void __wt_gen_init(WT_SESSION_IMPL *session);
extern void __wt_gen_next_drain(WT_SESSION_IMPL *session, int which);
extern void __wt_hash_map_destroy(WT_SESSION_IMPL *session, WT_HASH_MAP **hash_mapp);
extern void __wt_hash_map_unlock(
  WT_SESSION_IMPL *session, WT_HASH_MAP *hash_map, const void *key, size_t key_size);
extern void __wt_hazard_close(WT_SESSION_IMPL *session);
extern void __wt_hs_close(WT_SESSION_IMPL *session);
extern void __wt_hs_upd_time_window(WT_CURSOR *hs_cursor, WT_TIME_WINDOW **twp);
extern void __wt_json_close(WT_SESSION_IMPL *session, WT_CURSOR *cursor);
extern void __wt_layered_table_manager_remove_table(WT_SESSION_IMPL *session, uint32_t ingest_id);
extern void __wt_meta_track_discard(WT_SESSION_IMPL *session);
extern void __wt_meta_track_sub_on(WT_SESSION_IMPL *session);
extern void __wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_optrack_flush_buffer(WT_SESSION_IMPL *s);
extern void __wt_optrack_record_funcid(
  WT_SESSION_IMPL *session, const char *func, uint16_t *func_idp);
extern void __wt_os_stdio(WT_SESSION_IMPL *session);
extern void __wt_page_block_meta_assign(WT_SESSION_IMPL *session, WT_PAGE_BLOCK_META *meta);
extern void __wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep);
extern void __wt_random_init(WT_SESSION_IMPL *session, WT_RAND_STATE *rnd_state)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_random_init_default(WT_RAND_STATE *rnd_state)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_random_init_seed(WT_RAND_STATE *rnd_state, uint64_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_ref_addr_free(WT_SESSION_IMPL *session, WT_REF *ref);
extern void __wt_ref_out(WT_SESSION_IMPL *session, WT_REF *ref);
extern void __wt_root_ref_init(
  WT_SESSION_IMPL *session, WT_REF *root_ref, WT_PAGE *root, bool is_recno);
extern void __wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_schema_close_layered(WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered);
extern void __wt_scr_discard(WT_SESSION_IMPL *session);
extern void __wt_session_close_cache(WT_SESSION_IMPL *session);
extern void __wt_session_dhandle_sweep(WT_SESSION_IMPL *session);
extern void __wt_session_dhandle_writeunlock(WT_SESSION_IMPL *session);
extern void __wt_session_gen_enter(WT_SESSION_IMPL *session, int which);
extern void __wt_session_gen_leave(WT_SESSION_IMPL *session, int which);
extern void __wt_session_reset_last_error(WT_SESSION_IMPL *session);
extern void __wt_session_rng_init_once(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((visibility("default")));
extern void __wt_session_set_last_error(
  WT_SESSION_IMPL *session, int err, int sub_level_err, const char *fmt, ...);
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
extern void __wt_stat_session_clear_single(WT_SESSION_STATS *stats);
extern void __wt_stat_session_init_single(WT_SESSION_STATS *stats);
extern void __wt_thread_group_start_one(
  WT_SESSION_IMPL *session, WT_THREAD_GROUP *group, bool is_locked);
extern void __wt_thread_group_stop_one(WT_SESSION_IMPL *session, WT_THREAD_GROUP *group);
extern void __wt_tiered_flush_work_wait(WT_SESSION_IMPL *session, uint32_t timeout);
extern void __wt_tiered_get_flush(
  WT_SESSION_IMPL *session, uint64_t generation, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_get_flush_finish(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_get_remove_local(
  WT_SESSION_IMPL *session, uint64_t now, WT_TIERED_WORK_UNIT **entryp);
extern void __wt_tiered_remove_work(WT_SESSION_IMPL *session, WT_TIERED *tiered, bool locked);
extern void __wt_tiered_requeue_work(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT *entry);
extern void __wt_tiered_work_free(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT *entry);
extern void __wt_timestamp_to_hex_string(wt_timestamp_t ts, char *hex_timestamp);
extern void __wt_txn_bump_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_close_checkpoint_cursor(WT_SESSION_IMPL *session, WT_TXN **txn_arg);
extern void __wt_txn_destroy(WT_SESSION_IMPL *session);
extern void __wt_txn_get_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_global_destroy(WT_SESSION_IMPL *session);
extern void __wt_txn_op_free(WT_SESSION_IMPL *session, WT_TXN_OP *op);
extern void __wt_txn_release_resources(WT_SESSION_IMPL *session);
extern void __wt_txn_release_snapshot(WT_SESSION_IMPL *session);
extern void __wt_txn_snapshot_release_and_restore(WT_SESSION_IMPL *session);
extern void __wt_txn_stats_update(WT_SESSION_IMPL *session);
extern void __wt_txn_truncate_end(WT_SESSION_IMPL *session);
extern void __wt_update_obsolete_check(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd);
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
extern void __wt_verbose_worker_id(
  WT_SESSION_IMPL *session, const WT_VERBOSE_MESSAGE_INFO *verb_info, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4))) WT_GCC_FUNC_DECL_ATTRIBUTE((cold));
extern void __wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l);
extern void __wti_blkcache_get(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size,
  WT_BLKCACHE_ITEM **blkcache_retp, bool *foundp, bool *skip_cache_putp);
extern void __wti_blkcache_get_read_handle(WT_BLOCK *block);
extern void __wti_block_ckpt_destroy(WT_SESSION_IMPL *session, WT_BLOCK_CKPT *ci);
extern void __wti_block_configure_first_fit(WT_BLOCK *block, bool on);
extern void __wti_block_disagg_header_byteswap_copy(
  WT_BLOCK_DISAGG_HEADER *from, WT_BLOCK_DISAGG_HEADER *to);
extern void __wti_block_disagg_stat(
  WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, WT_DSRC_STATS *stats);
extern void __wti_block_ext_free(WT_SESSION_IMPL *session, WT_EXT **ext);
extern void __wti_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el);
extern void __wti_block_size_free(WT_SESSION_IMPL *session, WT_SIZE **sz);
extern void __wti_bm_method_set(WT_BM *bm, bool readonly);
extern void __wti_btcur_iterate_setup(WT_CURSOR_BTREE *cbt);
extern void __wti_ckpt_verbose(WT_SESSION_IMPL *session, WT_BLOCK *block, const char *tag,
  const char *ckpt_name, const uint8_t *ckpt_string, size_t ckpt_size);
extern void __wti_connection_destroy(WT_CONNECTION_IMPL *conn);
extern void __wti_cursor_reopen(WT_CURSOR *cursor, WT_DATA_HANDLE *dhandle);
extern void __wti_cursor_set_key_notsup(WT_CURSOR *cursor, ...);
extern void __wti_cursor_set_notsup(WT_CURSOR *cursor);
extern void __wti_cursor_set_value_notsup(WT_CURSOR *cursor, ...);
extern void __wti_free_ref(WT_SESSION_IMPL *session, WT_REF *ref, int page_type, bool free_pages);
extern void __wti_free_ref_index(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_INDEX *pindex, bool free_pages);
extern void __wti_read_row_time_window(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_TIME_WINDOW *tw);
extern void __wti_ref_addr_safe_free(WT_SESSION_IMPL *session, void *p, size_t len);
extern void __wti_rts_pop_work(WT_SESSION_IMPL *session, WT_RTS_WORK_UNIT **entryp);
extern void __wti_rts_progress_msg(WT_SESSION_IMPL *session, WT_TIMER *rollback_start,
  uint64_t rollback_count, uint64_t max_count, uint64_t *rollback_msg_count, bool walk);
extern void __wti_rts_work_free(WT_SESSION_IMPL *session, WT_RTS_WORK_UNIT *entry);
extern void __wti_schema_destroy_colgroup(WT_SESSION_IMPL *session, WT_COLGROUP **colgroupp);
extern void __wti_tiered_get_remove_shared(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp);
extern void __wti_txn_clear_durable_timestamp(WT_SESSION_IMPL *session);
extern void __wti_txn_clear_read_timestamp(WT_SESSION_IMPL *session);
extern void __wti_txn_get_pinned_timestamp(
  WT_SESSION_IMPL *session, wt_timestamp_t *tsp, uint32_t flags);
extern void __wti_txn_update_pinned_timestamp(WT_SESSION_IMPL *session, bool force);
static WT_INLINE WT_BTREE *__wt_curhs_get_btree(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_CELL *__wt_cell_leaf_value_parse(WT_PAGE *page, WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_CURSOR_BTREE *__wt_curhs_get_cbt(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_FILE_SYSTEM *__wt_fs_file_system(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_IKEY *__wt_ref_key_instantiated(WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_VISIBLE_TYPE __wt_txn_tw_start_visible(
  WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_VISIBLE_TYPE __wt_txn_tw_stop_visible(
  WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE WT_VISIBLE_TYPE __wt_txn_upd_visible_type(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_block_eligible_for_sweep(WT_BM *bm, WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_btree_syncing_by_other_session(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_cache_full(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_checksum_match(const void *chunk, size_t len, uint32_t v)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_clayered_deleted(const WT_ITEM *item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_conf_get_compiled(WT_CONNECTION_IMPL *conn, const char *config,
  WT_CONF **confp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_conf_is_compiled(WT_CONNECTION_IMPL *conn, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_cursor_has_cached_memory(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_delta_cell_type_visible_all(WT_CELL_UNPACK_DELTA_INT *unpack_delta)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_evict_page_soon_check(WT_SESSION_IMPL *session, WT_REF *ref,
  bool *inmem_split) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_failpoint(WT_SESSION_IMPL *session, uint64_t conn_flag,
  u_int probability) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_isalnum(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_isalpha(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_isascii(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_isdigit(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_isprint(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_isspace(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_leaf_page_can_split(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_off_page(WT_PAGE *page, const void *p)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_op_timer_fired(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_can_evict(WT_SESSION_IMPL *session, WT_REF *ref, bool *inmem_splitp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_del_committed_set(WT_PAGE_DELETED *page_del)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_del_visible(WT_SESSION_IMPL *session, WT_PAGE_DELETED *page_del,
  bool hide_prepared) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_del_visible_all(WT_SESSION_IMPL *session, WT_PAGE_DELETED *page_del,
  bool hide_prepared) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_evict_clean(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_evict_retry(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_is_empty(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_is_modified(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_is_reconciling(WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_page_materialization_check(
  WT_SESSION_IMPL *session, uint64_t rec_lsn_max) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_ref_addr_copy(WT_SESSION_IMPL *session, WT_REF *ref, WT_ADDR_COPY *copy)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_ref_is_root(WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_row_leaf_value(WT_PAGE *page, WT_ROW *rip, WT_ITEM *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_row_leaf_value_is_encoded(WT_ROW *rip)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_session_can_wait(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_spin_locked(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_spin_owned(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_split_descent_race(WT_SESSION_IMPL *session, WT_REF *ref,
  WT_PAGE_INDEX *saved_pindex) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_has_newest_and_visible_all(WT_SESSION_IMPL *session, uint64_t id,
  wt_timestamp_t timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_log_op_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_read_committed_should_release_snapshot(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_snap_min_visible(
  WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp, wt_timestamp_t durable_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_timestamp_visible(WT_SESSION_IMPL *session, wt_timestamp_t timestamp,
  wt_timestamp_t durable_timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_timestamp_visible_all(WT_SESSION_IMPL *session,
  wt_timestamp_t timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_tw_start_visible_all(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_tw_stop_visible_all(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_upd_value_visible_all(WT_SESSION_IMPL *session,
  WT_UPDATE_VALUE *upd_value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_upd_visible(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_upd_visible_all(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_visible(
  WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp, wt_timestamp_t durable_timestamp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_visible_all(WT_SESSION_IMPL *session, uint64_t id,
  wt_timestamp_t timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wt_txn_visible_id_snapshot(
  uint64_t id, uint64_t snap_min, uint64_t snap_max, uint64_t *snapshot, uint32_t snapshot_count)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE const char *__wt_page_type_str(uint8_t val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE const char *__wt_prepare_state_str(uint8_t val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE const char *__wt_update_type_str(uint8_t val)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_4b_pack_posint1(uint8_t **pp, uint8_t *end, uint64_t x1)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_4b_pack_posint2(uint8_t **pp, uint8_t *end, uint64_t x1, uint64_t x2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_4b_unpack_posint1(const uint8_t **pp, const uint8_t *end, uint64_t *x1)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_4b_unpack_posint2(const uint8_t **pp, const uint8_t *end, uint64_t *x1,
  uint64_t *x2) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_btcur_bounds_early_exit(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  bool next, bool *key_out_of_boundsp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_btcur_skip_page(WT_SESSION_IMPL *session, WT_REF *ref, void *context,
  bool visible_all, bool *skipp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_btree_block_free(WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_btree_shared(WT_SESSION_IMPL *session, const char *uri,
  const char **bt_cfg, bool *shared) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_btree_shared_base_name(WT_SESSION_IMPL *session, const char **namep,
  const char **checkpointp, WT_ITEM **name_bufp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_extend(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_init(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_initsize(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_set(WT_SESSION_IMPL *session, WT_ITEM *buf, const void *data,
  size_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_set_and_grow(WT_SESSION_IMPL *session, WT_ITEM *buf, const void *data,
  size_t size, size_t buf_size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_buf_setstr(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *s)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_cell_pack_value_match(WT_CELL *page_cell, WT_CELL *val_cell,
  const uint8_t *val_data, bool *matchp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_cell_unpack_safe(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL *cell, WT_CELL_UNPACK_ADDR *unpack_addr, WT_CELL_UNPACK_KV *unpack_value, const void *end)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_check_addr_validity(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta,
  bool expected_error) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_col_append_serial(WT_SESSION_IMPL *session, WT_PAGE *page,
  WT_INSERT_HEAD *ins_head, WT_INSERT ***ins_stack, WT_INSERT **new_insp, size_t new_ins_size,
  uint64_t *recnop, u_int skipdepth, bool exclusive)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_compare(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
  const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_compare_bounds(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_ITEM *key,
  uint64_t recno, bool upper, bool *key_out_of_bounds)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_compare_skip(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
  const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp, size_t *matchp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_conf_check_choice(
  WT_SESSION_IMPL *session, const char **choices, const char *str, size_t len, const char **result)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_conf_check_one(WT_SESSION_IMPL *session, const WT_CONFIG_CHECK *check,
  WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_conf_gets_def_func(WT_SESSION_IMPL *session, const WT_CONF *conf,
  uint64_t keys, int def, WT_CONFIG_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_curindex_get_valuev(WT_CURSOR *cursor, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_cursor_func_init(WT_CURSOR_BTREE *cbt, bool reenter)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_cursor_localkey(WT_CURSOR *cursor)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_curtable_get_valuev(WT_CURSOR *cursor, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_dsk_cell_data_ref_addr(WT_SESSION_IMPL *session,
  WT_CELL_UNPACK_ADDR *unpack, WT_ITEM *store) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_dsk_cell_data_ref_kv(WT_SESSION_IMPL *session, WT_CELL_UNPACK_KV *unpack,
  WT_ITEM *store) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_extlist_read_pair(const uint8_t **p, wt_off_t *offp, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_extlist_write_pair(uint8_t **p, wt_off_t off, wt_off_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fclose(WT_SESSION_IMPL *session, WT_FSTREAM **fstrp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fextend(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fflush(WT_SESSION_IMPL *session, WT_FSTREAM *fstr)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_file_lock(WT_SESSION_IMPL *session, WT_FH *fh, bool lock)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fprintf(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_directory_list(
  WT_SESSION_IMPL *session, const char *dir, const char *prefix, char ***dirlistp, u_int *countp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_directory_list_free(WT_SESSION_IMPL *session, char ***dirlistp,
  u_int count) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_directory_list_single(
  WT_SESSION_IMPL *session, const char *dir, const char *prefix, char ***dirlistp, u_int *countp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_remove(WT_SESSION_IMPL *session, const char *name, bool durable,
  bool locked) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_rename(WT_SESSION_IMPL *session, const char *from, const char *to,
  bool durable) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fs_size(WT_SESSION_IMPL *session, const char *name, wt_off_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_fsync(WT_SESSION_IMPL *session, WT_FH *fh, bool block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_getline(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_insert_serial(WT_SESSION_IMPL *session, WT_PAGE *page,
  WT_INSERT_HEAD *ins_head, WT_INSERT ***ins_stack, WT_INSERT **new_insp, size_t new_ins_size,
  u_int skipdepth, bool exclusive) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_lex_compare_short(const WT_ITEM *user_item, const WT_ITEM *tree_item)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_lex_compare_skip(WT_SESSION_IMPL *session, const WT_ITEM *user_item,
  const WT_ITEM *tree_item, size_t *matchp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_op_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd, WT_TXN_OP *op)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_pack_fixed_uint32(uint8_t **pp, size_t maxlen, uint32_t value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_page_cell_data_ref_kv(WT_SESSION_IMPL *session, WT_PAGE *page,
  WT_CELL_UNPACK_KV *unpack, WT_ITEM *store) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_page_dirty_and_evict_soon(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_page_modify_init(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_page_parent_modify_set(WT_SESSION_IMPL *session, WT_REF *ref,
  bool page_only) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_page_release(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_page_swap_func(
  WT_SESSION_IMPL *session, WT_REF *held, WT_REF *want, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
  ) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_pending_prepared_next_op(
  WT_SESSION_IMPL *session, WT_TXN_OP **opp, WT_PENDING_PREPARED_ITEM *prepared_item, WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_read(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len,
  void *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_ref_block_free(WT_SESSION_IMPL *session, WT_REF *ref,
  bool page_replacement) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_row_leaf_key(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip,
  WT_ITEM *key, bool instantiate) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_row_leaf_key_instantiate(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_snprintf(char *buf, size_t size, const char *fmt, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 3, 4)))
    WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_snprintf_len_incr(char *buf, size_t size, size_t *retsizep,
  const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 4, 5)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_snprintf_len_set(char *buf, size_t size, size_t *retsizep,
  const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 4, 5)))
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_spin_trylock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_spin_trylock_track(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_strcat(char *dest, size_t size, const char *src)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_struct_packv(WT_SESSION_IMPL *session, void *buffer, size_t size,
  const char *fmt, va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_struct_sizev(WT_SESSION_IMPL *session, size_t *sizep, const char *fmt,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_struct_unpackv(WT_SESSION_IMPL *session, const void *buffer, size_t size,
  const char *fmt, va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_sync_and_rename(WT_SESSION_IMPL *session, WT_FSTREAM **fstrp,
  const char *from, const char *to) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_activity_check(WT_SESSION_IMPL *session, bool *txn_active)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_autocommit_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_begin(WT_SESSION_IMPL *session, WT_CONF *conf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_claim_prepared_txn(WT_SESSION_IMPL *session, uint64_t prepared_id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_context_check(WT_SESSION_IMPL *session, bool requires_txn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_context_prepare_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_id_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_idle_cache_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_modify_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *upd, wt_timestamp_t *prev_tsp, u_int modify_type)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_modify_page_delete(WT_SESSION_IMPL *session, WT_REF *ref)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_op_delete_commit(WT_SESSION_IMPL *session, WT_TXN_OP *op,
  bool validate, bool assign_timestamp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_op_set_key(WT_SESSION_IMPL *session, const WT_ITEM *key)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_op_set_timestamp(WT_SESSION_IMPL *session, WT_TXN_OP *op,
  bool validate) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_read(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key,
  uint64_t recno, WT_UPDATE *upd) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_read_upd_list(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_ITEM *key, uint64_t recno, WT_UPDATE *upd) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_read_upd_list_internal(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_ITEM *key, uint64_t recno, WT_UPDATE *upd, WT_UPDATE **prepare_updp, WT_UPDATE **restored_updp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_search_check(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_txn_timestamp_usage_check(
  WT_SESSION_IMPL *session, WT_TXN_OP *op, wt_timestamp_t op_ts, wt_timestamp_t prev_op_durable_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_unpack_fixed_uint32(const uint8_t **pp, size_t maxlen, uint32_t *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_upd_alloc(WT_SESSION_IMPL *session, const WT_ITEM *value,
  u_int modify_type, WT_UPDATE **updp, size_t *sizep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_upd_alloc_tombstone(WT_SESSION_IMPL *session, WT_UPDATE **updp,
  size_t *sizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_update_serial(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_PAGE *page, WT_UPDATE **srch_upd, WT_UPDATE **updp, size_t upd_size, bool exclusive)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vfprintf(WT_SESSION_IMPL *session, WT_FSTREAM *fstr, const char *fmt,
  va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vpack_int(uint8_t **pp, size_t maxlen, int64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vpack_negint(uint8_t **pp, size_t maxlen, uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vpack_posint(uint8_t **pp, size_t maxlen, uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vpack_uint(uint8_t **pp, size_t maxlen, uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vsnprintf_len_set(char *buf, size_t size, size_t *retsizep,
  const char *fmt, va_list ap) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vunpack_int(const uint8_t **pp, size_t maxlen, int64_t *xp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vunpack_negint(const uint8_t **pp, size_t maxlen, uint64_t *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vunpack_posint(const uint8_t **pp, size_t maxlen, uint64_t *retp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_vunpack_uint(const uint8_t **pp, size_t maxlen, uint64_t *xp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_write(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len,
  const void *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int64_t __wt_decode_positive_as_signed(uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_4b_size_posint1(uint64_t x1)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_4b_size_posint2(uint64_t x1, uint64_t x2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_addr(WT_SESSION_IMPL *session, WT_CELL *cell,
  u_int cell_type, uint64_t recno, WT_PAGE_DELETED *page_del, WT_TIME_AGGREGATE *ta, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_copy(WT_SESSION_IMPL *session, WT_CELL *cell,
  WT_TIME_WINDOW *tw, uint64_t rle, uint64_t v) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_del(WT_SESSION_IMPL *session, WT_CELL *cell,
  WT_TIME_WINDOW *tw, uint64_t rle) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_int_key(WT_CELL *cell, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_leaf_key(WT_CELL *cell, uint8_t prefix, size_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_ovfl(WT_SESSION_IMPL *session, WT_CELL *cell, uint8_t type,
  WT_TIME_WINDOW *tw, uint64_t rle, size_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_pack_value(WT_SESSION_IMPL *session, WT_CELL *cell,
  WT_TIME_WINDOW *tw, uint64_t rle, size_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_cell_total_len(void *unpack_arg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_strnlen(const char *s, size_t maxlen)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_update_list_memsize(WT_UPDATE *upd)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_vsize_int(int64_t x) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_vsize_negint(uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_vsize_posint(uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE size_t __wt_vsize_uint(uint64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE u_char __wt_hex(int c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE u_char __wt_tolower(u_char c) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE u_int __wt_block_header(WT_BLOCK *block)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE u_int __wt_cell_type(WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE u_int __wt_cell_type_raw(WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE u_int __wt_skip_choose_depth(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_btree_bytes_evictable(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_btree_bytes_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_btree_bytes_updates(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_btree_dirty_intl_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_btree_dirty_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_btree_dirty_leaf_inuse(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_bytes_delta_updates(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_bytes_image(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_bytes_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_bytes_other(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_bytes_plus_overhead(WT_CACHE *cache, uint64_t sz)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_bytes_updates(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_dirty_intl_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_dirty_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_dirty_leaf_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cache_pages_inuse(WT_CACHE *cache)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_cell_rle(WT_CELL_UNPACK_KV *unpack)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_clock(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_clock_to_nsec(uint64_t end, uint64_t begin)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_encode_signed_as_positive(int64_t x)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_gen(WT_SESSION_IMPL *session, int which)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_rdtsc(void) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_safe_sub(uint64_t v1, uint64_t v2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_session_gen(WT_SESSION_IMPL *session, int which)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_txn_id_alloc(WT_SESSION_IMPL *session, bool publish)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint64_t __wt_txn_oldest_id(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE void __wt_block_header_byteswap(WT_BLOCK_HEADER *blk);
static WT_INLINE void __wt_block_header_byteswap_copy(WT_BLOCK_HEADER *from, WT_BLOCK_HEADER *to);
static WT_INLINE void __wt_btree_disable_bulk(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_buf_free(WT_SESSION_IMPL *session, WT_ITEM *buf);
static WT_INLINE void __wt_cache_decr_check_size(
  WT_SESSION_IMPL *session, size_t *vp, size_t v, const char *fld);
static WT_INLINE void __wt_cache_decr_check_uint64(
  WT_SESSION_IMPL *session, uint64_t *vp, uint64_t v, const char *fld);
static WT_INLINE void __wt_cache_dirty_decr(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_cache_dirty_incr(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_cache_page_byte_dirty_decr(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static WT_INLINE void __wt_cache_page_byte_updates_decr(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static WT_INLINE void __wt_cache_page_image_decr(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_cache_page_image_incr(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_cache_page_inmem_decr(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static WT_INLINE void __wt_cache_page_inmem_decr_delta_updates(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static WT_INLINE void __wt_cache_page_inmem_incr(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size, bool new_update);
static WT_INLINE void __wt_cache_page_inmem_incr_delta_updates(
  WT_SESSION_IMPL *session, WT_PAGE *page, size_t size);
static WT_INLINE void __wt_cell_get_ta(WT_CELL_UNPACK_ADDR *unpack_addr, WT_TIME_AGGREGATE **tap);
static WT_INLINE void __wt_cell_get_tw(WT_CELL_UNPACK_KV *unpack_value, WT_TIME_WINDOW **twp);
static WT_INLINE void __wt_cell_type_reset(
  WT_SESSION_IMPL *session, WT_CELL *cell, u_int old_type, u_int new_type);
static WT_INLINE void __wt_cell_unpack_addr(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL *cell, WT_CELL_UNPACK_ADDR *unpack_addr);
static WT_INLINE void __wt_cell_unpack_delta_leaf(WT_SESSION_IMPL *session,
  const WT_PAGE_HEADER *dsk, WT_DELTA_CELL_LEAF *cell, WT_CELL_UNPACK_DELTA_LEAF *unpack);
static WT_INLINE void __wt_cell_unpack_kv(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL *cell, WT_CELL_UNPACK_KV *unpack_value);
static WT_INLINE void __wt_cond_wait(
  WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs, bool (*run_func)(WT_SESSION_IMPL *));
static WT_INLINE void __wt_cursor_bound_reset(WT_CURSOR *cursor);
static WT_INLINE void __wt_cursor_dhandle_decr_use(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_cursor_dhandle_incr_use(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_cursor_free_cached_memory(WT_CURSOR *cursor);
static WT_INLINE void __wt_epoch(WT_SESSION_IMPL *session, struct timespec *tsp);
static WT_INLINE void __wt_gen_next(WT_SESSION_IMPL *session, int which, uint64_t *genp);
static WT_INLINE void __wt_milliseconds(WT_SESSION_IMPL *session, uint64_t *millisecondsp);
static WT_INLINE void __wt_modifies_max_memsize(WT_UPDATE_VECTOR *modifies,
  const char *value_format, size_t base_value_size, size_t *max_memsize);
static WT_INLINE void __wt_modify_max_memsize(
  const void *modify, size_t base_value_size, size_t *max_memsize);
static WT_INLINE void __wt_modify_max_memsize_format(
  const void *modify, const char *value_format, size_t base_value_size, size_t *max_memsize);
static WT_INLINE void __wt_modify_max_memsize_unpacked(WT_MODIFY *entries, int nentries,
  const char *value_format, size_t base_value_size, size_t *max_memsize);
static WT_INLINE void __wt_op_timer_start(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_op_timer_stop(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_page_modify_clear(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_page_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_page_modify_update_timestamp(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_page_only_modify_set(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wt_ref_ascend(
  WT_SESSION_IMPL *session, WT_REF **refp, WT_PAGE_INDEX **pindexp, uint32_t *slotp);
static WT_INLINE void __wt_ref_index_slot(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_INDEX **pindexp, uint32_t *slotp);
static WT_INLINE void __wt_ref_key(WT_PAGE *page, WT_REF *ref, void *keyp, size_t *sizep);
static WT_INLINE void __wt_ref_key_clear(WT_REF *ref);
static WT_INLINE void __wt_ref_key_onpage_set(
  WT_PAGE *page, WT_REF *ref, WT_CELL_UNPACK_ADDR *unpack);
static WT_INLINE void __wt_row_leaf_key_free(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip);
static WT_INLINE void __wt_row_leaf_key_info(WT_PAGE *page, void *copy, WT_IKEY **ikeyp,
  WT_CELL **cellp, void *datap, size_t *sizep, uint8_t *prefixp);
static WT_INLINE void __wt_row_leaf_key_set(WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK_KV *unpack);
static WT_INLINE void __wt_row_leaf_value_cell(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_CELL_UNPACK_KV *vpack);
static WT_INLINE void __wt_row_leaf_value_set(WT_ROW *rip, WT_CELL_UNPACK_KV *unpack);
static WT_INLINE void __wt_scr_free(WT_SESSION_IMPL *session, WT_ITEM **bufp);
static WT_INLINE void __wt_seconds(WT_SESSION_IMPL *session, uint64_t *secondsp);
static WT_INLINE void __wt_seconds32(WT_SESSION_IMPL *session, uint32_t *secondsp);
static WT_INLINE void __wt_spin_backoff(uint64_t *yield_count, uint64_t *sleep_usecs);
static WT_INLINE void __wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static WT_INLINE void __wt_spin_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static WT_INLINE void __wt_spin_lock_track(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static WT_INLINE void __wt_spin_unlock(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static WT_INLINE void __wt_spin_unlock_if_owned(WT_SESSION_IMPL *session, WT_SPINLOCK *t);
static WT_INLINE void __wt_struct_size_adjust(WT_SESSION_IMPL *session, size_t *sizep);
static WT_INLINE void __wt_timer_evaluate_ms(
  WT_SESSION_IMPL *session, WT_TIMER *start_time, uint64_t *time_diff_ms);
static WT_INLINE void __wt_timer_start(WT_SESSION_IMPL *session, WT_TIMER *start_time);
static WT_INLINE void __wt_timing_stress(
  WT_SESSION_IMPL *session, uint64_t flag, struct timespec *tsp);
static WT_INLINE void __wt_timing_stress_sleep_random(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_tree_modify_set(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_txn_cursor_op(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_txn_err_set(WT_SESSION_IMPL *session, int ret);
static WT_INLINE void __wt_txn_op_delete_apply_prepare_state(
  WT_SESSION_IMPL *session, WT_TXN_OP *op, bool commit);
static WT_INLINE void __wt_txn_op_set_recno(WT_SESSION_IMPL *session, uint64_t recno);
static WT_INLINE void __wt_txn_pinned_stable_timestamp(
  WT_SESSION_IMPL *session, wt_timestamp_t *pinned_stable_tsp);
static WT_INLINE void __wt_txn_pinned_timestamp(
  WT_SESSION_IMPL *session, wt_timestamp_t *pinned_tsp);
static WT_INLINE void __wt_txn_read_last(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_txn_unmodify(WT_SESSION_IMPL *session);
static WT_INLINE void __wt_upd_value_assign(WT_UPDATE_VALUE *upd_value, WT_UPDATE *upd);
static WT_INLINE void __wt_upd_value_clear(WT_UPDATE_VALUE *upd_value);
static WT_INLINE void __wt_usec_to_timespec(time_t usec, struct timespec *tsp);
static inline bool __wt_get_page_modify_ta(WT_SESSION_IMPL *session, WT_PAGE *page,
  WT_TIME_AGGREGATE **ta) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#ifdef HAVE_UNITTEST
extern WT_EXT *__ut_block_off_srch_last(WT_EXT **head, WT_EXT ***stack)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __ut_block_first_srch(WT_SESSION_IMPL *session, WT_EXT **head, wt_off_t size,
  WT_EXT ***stack) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __ut_block_off_match(WT_EXTLIST *el, wt_off_t off, wt_off_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int WT_CDECL __ut_txn_mod_compare(const void *a, const void *b)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_append(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t off, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_ext_alloc(WT_SESSION_IMPL *session, WT_EXT **extp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_ext_discard(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_ext_insert(WT_SESSION_IMPL *session, WT_EXTLIST *el, WT_EXT *ext)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_ext_prealloc(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_extend(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t *offp, wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_manager_session_cleanup(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_merge(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t off,
  wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_off_insert(WT_SESSION_IMPL *session, WT_EXTLIST *el, wt_off_t off,
  wt_off_t size) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_off_remove(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el,
  wt_off_t off, WT_EXT **extp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_size_discard(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_block_size_prealloc(WT_SESSION_IMPL *session, u_int max)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_chunkcache_bitmap_alloc(WT_SESSION_IMPL *session, size_t *bit_index)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_ckpt_mod_blkmod_entry(WT_SESSION_IMPL *session, WT_CKPT_BLOCK_MODS *blk_mod,
  wt_off_t offset, wt_off_t len) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_session_config_int(WT_SESSION_IMPL *session, const char *config)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __ut_block_off_srch(WT_EXT **head, wt_off_t off, WT_EXT ***stack, bool skip_off);
extern void __ut_block_off_srch_pair(
  WT_EXTLIST *el, wt_off_t off, WT_EXT **beforep, WT_EXT **afterp);
extern void __ut_block_size_srch(WT_SIZE **head, wt_off_t size, WT_SIZE ***stack);
extern void __ut_chunkcache_bitmap_free(WT_SESSION_IMPL *session, size_t bit_index);

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
