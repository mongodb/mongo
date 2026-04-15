/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_block_disagg_corrupt --
 *     Report a block has been corrupted, external API.
 */
int
__wti_block_disagg_corrupt(
  WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_BLOCK_DISAGG_ADDRESS_COOKIE root_cookie;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PAGE_BLOCK_META block_meta;

    /* Read the block. */
    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wti_block_disagg_read(bm, session, tmp, &block_meta, addr, addr_size));

    /* Crack the cookie, dump the block. */
    WT_ERR(__wt_block_disagg_addr_unpack(session, &addr, addr_size, &root_cookie));
    __wt_log_data_dump(session, tmp->data, tmp->size,
      "corrupt dump: {%" PRIu32 ": %" PRIuMAX ", %" PRIu32 ", %#" PRIx32 "}", (uint32_t)0,
      (uintmax_t)root_cookie.page_id, root_cookie.size, root_cookie.checksum);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __block_disagg_read_err --
 *     Print a block disagg read error context in a standard way.
 */
static void
__block_disagg_read_err(WT_SESSION_IMPL *session, const char *name, uint32_t size, uint64_t page_id,
  uint64_t lsn, bool is_delta, int32_t delta_seq, const char *context_msg_fmt, ...)
{
    WT_DECL_RET;

    char context_msg_src[256];
    const char *context_msg = context_msg_src;
    va_list args;
    va_start(args, context_msg_fmt);
    WT_ERR(__wt_vsnprintf(context_msg_src, sizeof(context_msg_src), context_msg_fmt, args));

    char page_desc[32];
    if (is_delta)
        WT_ERR(__wt_snprintf(page_desc, sizeof(page_desc), "delta page: %" PRId32, delta_seq));
    else
        WT_ERR(__wt_snprintf(page_desc, sizeof(page_desc), "base image"));

    if (0) {
err:
        /* If something went wrong, print the format string and drop parameters. */
        context_msg = context_msg_fmt;
        page_desc[0] = '\0';
    }
    va_end(args);

    __wt_errx(session,
      "%s: read error for %" PRIu32
      "B block at "
      "page %" PRIu64 ", lsn %" PRIu64 ", %s, %s",
      name, size, page_id, lsn, page_desc, context_msg);
}

/*
 * __block_disagg_check_lsn_frontier --
 *     Check that the LSN is not ahead of the materialization frontier.
 */
static void
__block_disagg_check_lsn_frontier(WT_SESSION_IMPL *session, uint64_t lsn)
{
    uint64_t last_materialized_lsn =
      __wt_atomic_load_uint64_acquire(&S2C(session)->disaggregated_storage.last_materialized_lsn);
    /* FIXME-WT-15914 Resolve special constant for materialization frontier LSN. */
    if (last_materialized_lsn != WT_DISAGG_LSN_NONE &&
      last_materialized_lsn != WT_DISAGG_START_LSN && lsn > last_materialized_lsn) {
        /* FIXME-WT-15818 Consider crashing upon this check failure. */
        WT_STAT_CONN_INCR(session, disagg_block_read_ahead_frontier);
        __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE,
          "LSN frontier warning: read LSN %" PRIu64
          " is ahead of the materialization frontier at LSN %" PRIu64
          " (this is not necessarily an error)",
          lsn, last_materialized_lsn);
    }
}

/*
 * __block_disagg_read_multiple --
 *     Read a full page along with its deltas, into multiple buffers. The page is referenced by a
 *     page id, checkpoint id pair.
 */
static int
__block_disagg_read_multiple(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg,
  WT_PAGE_BLOCK_META *block_meta, uint64_t page_id, uint64_t flags, uint64_t lsn, uint64_t base_lsn,
  uint32_t size, uint32_t checksum, WT_ITEM *results_array, uint32_t *results_count)
{
    WT_BLOCK_DISAGG_HEADER *blk, swap;
    WT_DECL_RET;
    WT_ITEM *current;
    WT_PAGE_LOG_GET_ARGS get_args;
    uint64_t time_start, time_stop;
    uint32_t retry, tmp_count, block_size_sum;
    int32_t last, result;
    uint8_t expected_magic;
    bool from_cache, is_delta;

    /* This variable is only used in an assertion, diagnostic builders don't like this. */
    WT_UNUSED(block_size_sum);
    block_size_sum = 0;
    from_cache = false;
    time_start = __wt_clock(session);

    WT_CLEAR(get_args);
    get_args.lsn = lsn;
    WT_ASSERT(session, block_meta != NULL);

    if (S2BT(session)->storage_tier == WT_BTREE_STORAGE_TIER_COLD)
        F_SET(&get_args, WT_PAGE_LOG_COLD);

    __wt_verbose(session, WT_VERB_READ,
      "page_id %" PRIu64 ", flags %" PRIx64 ", lsn %" PRIu64 ", base_lsn %" PRIu64 ", size %" PRIu32
      ", checksum %" PRIx32,
      page_id, flags, lsn, base_lsn, size, checksum);

    WT_STAT_CONN_INCR(session, disagg_block_get);
    WT_STAT_CONN_INCR(session, block_read);
    WT_STAT_CONN_INCRV(session, block_byte_read, size);
    __block_disagg_check_lsn_frontier(session, lsn);

    if (F_ISSET(block_disagg, WT_BLOCK_DISAGG_HS)) {
        WT_STAT_CONN_INCR(session, disagg_block_hs_get);
        WT_STAT_CONN_INCRV(session, disagg_block_hs_byte_read, size);
    }
    if (F_ISSET(&get_args, WT_PAGE_LOG_COLD))
        WT_STAT_CONN_INCR(session, disagg_block_get_cold);

    /*
     * If the page server returns no data but doesn't explicitly fail with an error, retry the read
     * a few times in case the issue is transient.
     *
     * FIXME: WT-15768: To support current testing, we never give up. It is better to hang here as
     * that will allow us to generate a core dump if desired. We should revisit this when we have
     * more complete end-to-end story for handling read failures.
     */
    for (retry = 0, tmp_count = 0; tmp_count == 0; retry++) {
        if (retry > 0) {
            __wt_verbose_notice(session, WT_VERB_READ,
              "retry #%" PRIu32 " for page_id %" PRIu64 ", flags %" PRIx64 ", lsn %" PRIu64
              ", base_lsn %" PRIu64 ", size %" PRIu32 ", checksum %" PRIx32,
              retry, page_id, flags, lsn, base_lsn, size, checksum);

            __wt_sleep(0, WT_MIN(10000 + retry * 5000, 500000));
        }

        tmp_count = *results_count;

        /*
         * Output buffers do not need to be pre-allocated, the PALI interface does that.
         */
        WT_ERR(block_disagg->plhandle->plh_get(block_disagg->plhandle, &session->iface, page_id, 0,
          &get_args, results_array, &tmp_count));

        WT_ASSERT(session, tmp_count <= WT_DELTA_LIMIT + 1);
    }

    *results_count = tmp_count;

    last = (int32_t)(*results_count - 1);

    /* Set the cumulative size from the cookie before the loop overwrites the size variable. */
    block_meta->cumulative_size = size;

    /*
     * Walk through all the results from most recent delta backwards to the base page. This makes it
     * easier to do checks.
     */
    for (result = last; result >= 0; result--) {
        current = &results_array[result];
        WT_ASSERT(session, current->size < UINT32_MAX);
        size = (uint32_t)current->size;
        is_delta = (result != 0);
        block_size_sum += size;

        if (is_delta)
            __wt_verbose(session, WT_VERB_READ,
              "Reading delta page at position #%" PRId32 " for page_id %" PRIu64, result, page_id);
        else
            __wt_verbose(session, WT_VERB_READ, "Reading base page for page_id %" PRIu64, page_id);

        /*
         * Do little- to big-endian handling early on.
         */
        blk = WT_BLOCK_HEADER_REF(current->data);
        __wti_block_disagg_header_byteswap_copy(blk, &swap);

        /*
         * TODO(WT-16511): When we have the original checksum stored in the page, we should check
         * that instead of skipping the check entirely for cached pages.
         */
        if (F_ISSET(&swap, WT_BLOCK_DISAGG_MODIFIED))
            from_cache = true;
        if (F_ISSET(&swap, WT_BLOCK_DISAGG_MODIFIED) || swap.checksum == checksum) {
            blk->checksum = 0;
            if (__wt_checksum_match(current->data,
                  F_ISSET(&swap, WT_BLOCK_DATA_CKSUM) ? size : WT_MIN(size, WT_BLOCK_COMPRESS_SKIP),
                  swap.checksum)) {
                expected_magic =
                  (is_delta ? WT_BLOCK_DISAGG_MAGIC_DELTA : WT_BLOCK_DISAGG_MAGIC_BASE);
                if (swap.magic != expected_magic) {
                    __block_disagg_read_err(session, block_disagg->name, size, page_id, lsn,
                      is_delta, result, "magic %" PRIu8 ": doesn't match expected magic of %" PRIu8,
                      swap.magic, expected_magic);
                    goto corrupt;
                }
                if (swap.compatible_version > WT_BLOCK_DISAGG_COMPATIBLE_VERSION) {
                    __block_disagg_read_err(session, block_disagg->name, size, page_id, lsn,
                      is_delta, result,
                      "compatible version error, version %" PRIu8
                      " is greater than compatible version of %" PRIu8,
                      swap.compatible_version, WT_BLOCK_DISAGG_COMPATIBLE_VERSION);
                    goto corrupt;
                }

                if (result == last) {
                    /* Set the other metadata returned by the Page Service. */
                    block_meta->page_id = page_id;
                    block_meta->backlink_lsn = get_args.backlink_lsn;
                    block_meta->base_lsn = get_args.base_lsn;
                    block_meta->disagg_lsn = get_args.lsn;
                    block_meta->delta_count = F_ISSET(&swap, WT_BLOCK_DISAGG_MODIFIED) ?
                      (uint8_t)get_args.delta_count :
                      (uint8_t)(*results_count - 1);
                    block_meta->checksum = checksum;

                    WT_ASSERT(session, get_args.lsn > 0);
                    if (!F_ISSET(&swap, WT_BLOCK_DISAGG_MODIFIED)) {
                        WT_ASSERT(session,
                          (*results_count > 1) ==
                            FLD_ISSET(flags, WT_BLOCK_DISAGG_ADDR_FLAG_DELTA));

                        /* The server is allowed to set base LSN to 0 for full page images. */
                        WT_ASSERT(session,
                          (get_args.base_lsn == 0 && *results_count == 1) ||
                            get_args.base_lsn == base_lsn);

                        if (block_meta->delta_count > 0)
                            WT_ASSERT(session, get_args.base_lsn > 0);
                        else
                            WT_ASSERT(
                              session, get_args.base_lsn == 0 && get_args.base_checkpoint_id == 0);
                    }
                }

                /*
                 * Swap the page-header as needed; this doesn't belong here, but it's the best place
                 * to catch all callers.
                 */
                __wt_page_header_byteswap((void *)current->data);
                checksum = swap.previous_checksum;
                continue;
            }

            if (!F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
                __block_disagg_read_err(session, block_disagg->name, size, page_id, lsn, is_delta,
                  result,
                  "calculated checksum of %" PRIx32 " doesn't match expected checksum of %" PRIx32,
                  swap.checksum, checksum);
        } else if (!F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
            __block_disagg_read_err(session, block_disagg->name, size, page_id, lsn, is_delta,
              result, "header checksum of %" PRIx32 " doesn't match expected checksum of %" PRIx32,
              swap.checksum, checksum);

corrupt:
        if (!F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
            __wt_log_data_dump(session, current->data, current->size,
              "corrupt dump: {%" PRIu32 ": %" PRIuMAX ", %" PRIu32 ", %#" PRIx32 "}", (uint32_t)0,
              (uintmax_t)page_id, size, checksum);

        /* Panic if a checksum fails during an ordinary read. */
        F_SET_ATOMIC_32(S2C(session), WT_CONN_DATA_CORRUPTION);
        if (F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
            WT_ERR(WT_ERROR);
        WT_ERR_PANIC(session, WT_ERROR, "%s: fatal read error", block_disagg->name);
    }

    /*
     * The size contained in the cookie must match the sum of all the individual block sizes,
     * however this isn't true when the victim cache is enabled and the read is serviced by the
     * cache. Effectively blocks stored in the victim cache are compressed versions of the in-memory
     * page which is different to what the cookie size tracks which refers to pages written to the
     * page service.
     */
    if (!from_cache)
        /*
         * Since the Victim Cache stores compressed variant of in-memory page representation rather
         * than what we have in SLS, these numbers will not match.
         */
        WT_ASSERT(session, block_meta->cumulative_size == block_size_sum);

err:
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_disaggbmread(session, WT_CLOCKDIFF_US(time_stop, time_start));

    return (ret);
}

/*
 * __wti_block_disagg_read --
 *     A basic read of a single block is not supported in disaggregated storage.
 */
int
__wti_block_disagg_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size)
{
    WT_UNUSED(bm);
    WT_UNUSED(session);
    WT_UNUSED(buf);
    WT_UNUSED(block_meta);
    WT_UNUSED(addr);
    WT_UNUSED(addr_size);

    return (ENOTSUP);
}

/*
 * __wti_block_disagg_read_multiple --
 *     Map or read address cookie referenced page and deltas into an array of buffers, with memory
 *     managed by a memory buffer.
 */
int
__wti_block_disagg_read_multiple(WT_BM *bm, WT_SESSION_IMPL *session,
  WT_PAGE_BLOCK_META *block_meta, const uint8_t *addr, size_t addr_size, WT_ITEM *buffer_array,
  u_int *buffer_count)
{
    WT_BLOCK_DISAGG *block_disagg;
    WT_BLOCK_DISAGG_ADDRESS_COOKIE cookie;

    block_disagg = (WT_BLOCK_DISAGG *)bm->block;

    /* Crack the cookie. */
    WT_RET(__wt_block_disagg_addr_unpack(session, &addr, addr_size, &cookie));

    /* Read the block. */
    WT_RET(
      __block_disagg_read_multiple(session, block_disagg, block_meta, cookie.page_id, cookie.flags,
        cookie.lsn, cookie.base_lsn, cookie.size, cookie.checksum, buffer_array, buffer_count));

    return (0);
}
