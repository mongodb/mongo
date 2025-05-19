/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>
#include <gcc.h>
#include <swap.h> /* for __wt_bswap64 */
#include "queue.h"

#include "palm_kv.h"
#include "palm_verbose.h"

/*
 * This page log implementation is used for demonstration and testing. All objects are stored as
 * local files in a designated directory.
 */

#ifdef __GNUC__
#if __GNUC__ > 7 || (__GNUC__ == 7 && __GNUC_MINOR__ > 0)
/*
 * !!!
 * GCC with -Wformat-truncation complains about calls to snprintf in this file.
 * There's nothing wrong, this makes the warning go away.
 */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#endif

#define PALM_KV_RET(palm, session, r)                                                              \
    {                                                                                              \
        int _ret = (r);                                                                            \
        if (_ret != 0)                                                                             \
            return (                                                                               \
              palm_kv_err(palm, session, _ret, "%s: %d: \"%s\": failed", __FILE__, __LINE__, #r)); \
    }

#define PALM_KV_ERR(palm, session, r)                                                            \
    {                                                                                            \
        ret = (r);                                                                               \
        if (ret != 0) {                                                                          \
            ret =                                                                                \
              palm_kv_err(palm, session, ret, "%s: %d: \"%s\": failed", __FILE__, __LINE__, #r); \
            goto err;                                                                            \
        }                                                                                        \
    }

/*
 * The default cache size for LMDB. Instead of changing this here, consider setting
 * cache_size_mb=.... when loading the extension library.
 */
#define DEFAULT_PALM_CACHE_SIZE_MB 500

/* Directory page log structure. */
typedef struct {
    WT_PAGE_LOG page_log; /* Must come first */

    WT_EXTENSION_API *wt_api; /* Extension API */

    char *kv_home;
    PALM_KV_ENV *kv_env;

    /* We use random for artificial delays */
    uint32_t rand_w, rand_z;

    /*
     * Locks are used to protect the file handle queue.
     */
    pthread_rwlock_t pl_handle_lock;

    /*
     * Keep the number of references to this page log.
     */
    uint32_t reference_count;

    uint32_t cache_size_mb;            /* Size of cache in megabytes */
    uint32_t delay_ms;                 /* Average length of delay when simulated */
    uint32_t error_ms;                 /* Average length of sleep when simulated */
    uint32_t force_delay;              /* Force a simulated network delay every N operations */
    uint32_t force_error;              /* Force a simulated network error every N operations */
    uint32_t materialization_delay_ms; /* Average length of materialization delay */
    uint64_t last_materialized_lsn;    /* The last materialized LSN (0 if not set) */
    uint32_t verbose;                  /* Verbose level */

    /*
     * Statistics are collected but not yet exposed.
     */
    uint64_t object_puts; /* (What would be) network writes */
    uint64_t object_gets; /* (What would be) network requests for data */

    /* Queue of file handles */
    TAILQ_HEAD(palm_handle_qh, palm_handle) fileq;

} PALM;

typedef struct palm_handle {
    WT_PAGE_LOG_HANDLE iface; /* Must come first */

    PALM *palm; /* Enclosing PALM  */
    uint64_t table_id;

    TAILQ_ENTRY(palm_handle) q; /* Queue of handles */
} PALM_HANDLE;

/*
 * Forward function declarations for internal functions
 */
static int palm_configure(PALM *, WT_CONFIG_ARG *);
static int palm_configure_int(
  PALM *, WT_CONFIG_PARSER *, WT_CONFIG_ARG *, const char *, uint32_t *);
static int palm_err(PALM *, WT_SESSION *, int, const char *, ...);
static int palm_kv_err(PALM *, WT_SESSION *, int, const char *, ...);
static void palm_init_context(PALM *, PALM_KV_CONTEXT *);

/*
 * Forward function declarations for page log API implementation
 */
static int palm_add_reference(WT_PAGE_LOG *);
static int palm_terminate(WT_PAGE_LOG *, WT_SESSION *);

/*
 * palm_configure
 *     Parse the configuration for the keys we care about.
 */
static int
palm_configure(PALM *palm, WT_CONFIG_ARG *config)
{
    WT_CONFIG_PARSER *env_parser;
    const char *env_config;
    int ret, t_ret;

    if ((env_config = getenv("WT_PALM_CONFIG")) == NULL)
        env_config = "";

    /* A null session is allowed. */
    if ((ret = palm->wt_api->config_parser_open(
           palm->wt_api, NULL, env_config, strlen(env_config), &env_parser)) != 0)
        goto err;

    palm->cache_size_mb = DEFAULT_PALM_CACHE_SIZE_MB;
    if ((ret = palm_configure_int(
           palm, env_parser, config, "cache_size_mb", &palm->cache_size_mb)) != 0)
        goto err;
    if ((ret = palm_configure_int(palm, env_parser, config, "delay_ms", &palm->delay_ms)) != 0)
        goto err;
    if ((ret = palm_configure_int(palm, env_parser, config, "error_ms", &palm->error_ms)) != 0)
        goto err;
    if ((ret = palm_configure_int(palm, env_parser, config, "force_delay", &palm->force_delay)) !=
      0)
        goto err;
    if ((ret = palm_configure_int(palm, env_parser, config, "force_error", &palm->force_error)) !=
      0)
        goto err;
    if ((ret = palm_configure_int(palm, env_parser, config, "materialization_delay_ms",
           &palm->materialization_delay_ms)) != 0)
        goto err;
    if ((ret = palm_configure_int(palm, env_parser, config, "verbose", &palm->verbose)) != 0)
        goto err;

err:
    if (env_parser != NULL) {
        t_ret = env_parser->close(env_parser);
        if (ret == 0)
            ret = t_ret;
    }
    return (ret);
}

/*
 * palm_configure_int
 *     Look for a particular configuration key, and return its integer value.
 */
static int
palm_configure_int(PALM *palm, WT_CONFIG_PARSER *env_parser, WT_CONFIG_ARG *config, const char *key,
  uint32_t *valuep)
{
    WT_CONFIG_ITEM v;
    int ret;

    ret = 0;

    /*
     * Environment configuration overrides configuration used with loading the library, so check
     * that first.
     */
    if ((ret = env_parser->get(env_parser, key, &v)) == 0 ||
      (ret = palm->wt_api->config_get(palm->wt_api, NULL, config, key, &v)) == 0) {
        if (v.len == 0 || v.type != WT_CONFIG_ITEM_NUM)
            ret = palm_err(palm, NULL, EINVAL, "force_error config arg: integer required");
        else
            *valuep = (uint32_t)v.val;
    } else if (ret == WT_NOTFOUND)
        ret = 0;
    else
        ret = palm_err(palm, NULL, EINVAL, "WT_API->config_get");

    return (ret);
}

/*
 * sleep_us --
 *     Sleep for the specified microseconds.
 */
static void
sleep_us(uint64_t us)
{
    struct timeval tv;

    /* Cast needed for some compilers that suspect the calculation can overflow (it can't). */
    tv.tv_sec = (time_t)(us / WT_MILLION);
    tv.tv_usec = (suseconds_t)(us % WT_MILLION);
    (void)select(0, NULL, NULL, NULL, &tv);
}

/*
 * palm_compute_delay_us --
 *     Compute a random delay around a given average. Use a uniform random distribution from 0.5 of
 *     the given delay to 1.5 of the given delay.
 */
static uint64_t
palm_compute_delay_us(PALM *palm, uint64_t avg_delay_us)
{
    uint32_t w, z, r;
    if (avg_delay_us == 0)
        return (0);

    /*
     * Note: this is WiredTiger's RNG algorithm. Since this module is packaged independent of
     * WiredTiger's internals, it's not feasible to call directly into its implementation.
     */
    w = palm->rand_w;
    z = palm->rand_z;
    if (w == 0 || z == 0) {
        palm->rand_w = w = 521288629;
        palm->rand_z = z = 362436069;
    }
    palm->rand_z = (36969 * (z & 65535) + (z >> 16)) & 0xffffffff;
    palm->rand_w = (18000 * (w & 65535) + (w >> 16)) & 0xffffffff;
    r = ((z << 16) + (w & 65535)) & 0xffffffff;

    return (avg_delay_us / 2 + r % avg_delay_us);
}

/*
 * palm_delay --
 *     Add any artificial delay or simulated network error during an object transfer.
 */
static int
palm_delay(PALM *palm)
{
    int ret;
    uint64_t us;

    ret = 0;
    if (palm->force_delay != 0 &&
      (palm->object_gets + palm->object_puts) % palm->force_delay == 0) {
        us = palm_compute_delay_us(palm, (uint64_t)palm->delay_ms * WT_THOUSAND);
        PALM_VERBOSE_PRINT(palm,
          "Artificial delay %" PRIu64 " microseconds after %" PRIu64 " object reads, %" PRIu64
          " object writes\n",
          us, palm->object_gets, palm->object_puts);
        sleep_us(us);
    }
    if (palm->force_error != 0 &&
      (palm->object_gets + palm->object_puts) % palm->force_error == 0) {
        us = palm_compute_delay_us(palm, (uint64_t)palm->error_ms * WT_THOUSAND);
        PALM_VERBOSE_PRINT(palm,
          "Artificial error returned after %" PRIu64 " microseconds sleep, %" PRIu64
          " object reads, %" PRIu64 " object writes\n",
          us, palm->object_gets, palm->object_puts);
        sleep_us(us);
        ret = ENETUNREACH;
    }

    return (ret);
}

/*
 * palm_err --
 *     Print errors from the interface. Returns "ret", the third argument.
 */
static int
palm_err(PALM *palm, WT_SESSION *session, int ret, const char *format, ...)
{
    va_list ap;
    WT_EXTENSION_API *wt_api;
    char buf[1000];

    va_start(ap, format);
    wt_api = palm->wt_api;
    if (vsnprintf(buf, sizeof(buf), format, ap) >= (int)sizeof(buf))
        wt_api->err_printf(wt_api, session, "palm: error overflow");
    wt_api->err_printf(
      wt_api, session, "palm: %s: %s", wt_api->strerror(wt_api, session, ret), buf);
    va_end(ap);

    return (ret);
}

/*
 * palm_kv_err --
 *     Print errors from the interface. Returns "ret", the third argument.
 */
static int
palm_kv_err(PALM *palm, WT_SESSION *session, int ret, const char *format, ...)
{
    va_list ap;
    WT_EXTENSION_API *wt_api;
    char buf[1000];
    const char *lmdb_error;

    va_start(ap, format);
    wt_api = palm->wt_api;
    if (vsnprintf(buf, sizeof(buf), format, ap) >= (int)sizeof(buf))
        wt_api->err_printf(wt_api, session, "palm: error overflow");
    lmdb_error = mdb_strerror(ret);
    wt_api->err_printf(wt_api, session, "palm LMDB: %s: %s", lmdb_error, buf);
    PALM_VERBOSE_PRINT(palm, "palm LMDB: %s: %s\n", lmdb_error, buf);
    va_end(ap);

    return (WT_ERROR);
}

/*
 * palm_resize_item --
 *     Resize a buffer as needed.
 */
static int
palm_resize_item(WT_ITEM *item, size_t new_size)
{
    if (item->memsize < new_size) {
        item->mem = realloc(item->mem, new_size);
        if (item->mem == NULL)
            return (errno);
        item->memsize = new_size;
    }
    item->data = item->mem;
    item->size = new_size;
    return (0);
}

/*
 * palm_init_context --
 *     Initialize a context in a standard way.
 */
static void
palm_init_context(PALM *palm, PALM_KV_CONTEXT *context)
{
    memset(context, 0, sizeof(*context));

    /*
     * To get more testing variation, we could call palm_compute_delay_us to randomize this number.
     * If we do so, we need to make sure items are materialized in the same order they are written.
     * So when setting PAGE_KEY.timestamp_materialized_us, we'd need to make each value set was
     * monotonically increasing.
     */
    context->materialization_delay_us = palm->materialization_delay_ms * WT_THOUSAND;
    context->last_materialized_lsn = palm->last_materialized_lsn;
}

/*
 * palm_add_reference --
 *     Add a reference to the page log so we can reference count to know when to really terminate.
 */
static int
palm_add_reference(WT_PAGE_LOG *page_log)
{
    PALM *palm;

    palm = (PALM *)page_log;

    /*
     * Missing reference or overflow?
     */
    if (palm->reference_count == 0 || palm->reference_count + 1 == 0)
        return (EINVAL);
    ++palm->reference_count;
    return (0);
}

/*
 * palm_begin_checkpoint --
 *     Begin a checkpoint.
 */
static int
palm_begin_checkpoint(WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t checkpoint_id)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    int ret;

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    PALM_KV_ERR(palm, session,
      palm_kv_put_global(&context, PALM_KV_GLOBAL_CHECKPOINT_STARTED, checkpoint_id));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

/*
 * palm_complete_checkpoint_ext --
 *     Complete a checkpoint.
 */
static int
palm_complete_checkpoint_ext(WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t checkpoint_id,
  uint64_t checkpoint_timestamp, const WT_ITEM *checkpoint_metadata, uint64_t *lsnp)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    uint64_t completed_checkpoint, lsn, started_checkpoint;
    int ret;

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    PALM_KV_ERR(palm, session,
      palm_kv_get_global(&context, PALM_KV_GLOBAL_CHECKPOINT_STARTED, &started_checkpoint));
    ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_CHECKPOINT_COMPLETED, &completed_checkpoint);
    if (ret == MDB_NOTFOUND) {
        /* This trips the first time we complete a checkpoint. */
        completed_checkpoint = 0;
        ret = 0;
    }
    PALM_KV_ERR(palm, session, ret);
    ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_REVISION, &lsn);
    if (ret == MDB_NOTFOUND) {
        lsn = 1;
        ret = 0;
    }
    PALM_KV_ERR(palm, session, ret);

    if (completed_checkpoint >= started_checkpoint) {
        ret = palm_err(palm, session, EINVAL, "complete checkpoint id that was never begun");
        goto err;
    }

    PALM_KV_ERR(palm, session,
      palm_kv_put_checkpoint(
        &context, lsn, checkpoint_id, checkpoint_timestamp, checkpoint_metadata));
    PALM_KV_ERR(palm, session, palm_kv_put_global(&context, PALM_KV_GLOBAL_REVISION, lsn + 1));
    PALM_KV_ERR(palm, session, palm_kv_put_global(&context, PALM_KV_GLOBAL_CHECKPOINT_STARTED, 0));
    PALM_KV_ERR(palm, session,
      palm_kv_put_global(&context, PALM_KV_GLOBAL_CHECKPOINT_COMPLETED, checkpoint_id));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    if (lsnp != NULL)
        *lsnp = lsn;
    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

/*
 * palm_complete_checkpoint --
 *     Complete a checkpoint.
 */
static int
palm_complete_checkpoint(WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t checkpoint_id)
{
    return (palm_complete_checkpoint_ext(page_log, session, checkpoint_id, 0, NULL, NULL));
}

/*
 * palm_get_complete_checkpoint --
 *     Get the last completed checkpoint id.
 */
static int
palm_get_complete_checkpoint(WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t *checkpoint_id)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    uint64_t kv_checkpoint;
    int ret;

    *checkpoint_id = 0;

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, true));
    PALM_KV_ERR(palm, session,
      palm_kv_get_global(&context, PALM_KV_GLOBAL_CHECKPOINT_COMPLETED, &kv_checkpoint));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    *checkpoint_id = kv_checkpoint;

    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

/*
 * palm_get_complete_checkpoint_ext --
 *     Get information about the most recently completed checkpoint.
 */
static int
palm_get_complete_checkpoint_ext(WT_PAGE_LOG *page_log, WT_SESSION *session,
  uint64_t *checkpoint_lsn, uint64_t *checkpoint_id, uint64_t *checkpoint_timestamp,
  WT_ITEM *checkpoint_metadata)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    void *metadata;
    size_t metadata_len;
    int ret;

    metadata = NULL;
    metadata_len = 0;
    if (checkpoint_lsn != NULL)
        *checkpoint_lsn = 0;
    if (checkpoint_id != NULL)
        *checkpoint_id = 0;
    if (checkpoint_timestamp != NULL)
        *checkpoint_timestamp = 0;
    if (checkpoint_metadata != NULL)
        memset(checkpoint_metadata, 0, sizeof(WT_ITEM));

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, true));

    ret = palm_kv_get_last_checkpoint(
      &context, checkpoint_lsn, checkpoint_id, checkpoint_timestamp, &metadata, &metadata_len);
    if (ret == MDB_NOTFOUND) {
        ret = WT_NOTFOUND;
        goto err;
    }
    PALM_KV_ERR(palm, session, ret);
    if (checkpoint_metadata != NULL) {
        PALM_KV_ERR(palm, session, palm_resize_item(checkpoint_metadata, metadata_len));
        memcpy(checkpoint_metadata->mem, metadata, metadata_len);
    }

    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));
    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

/*
 * palm_get_last_lsn --
 *     Get the last LSN.
 */
static int
palm_get_last_lsn(WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t *lsn)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    uint64_t kv_lsn;
    int ret;

    *lsn = 0;

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, true));
    PALM_KV_ERR(palm, session, palm_kv_get_global(&context, PALM_KV_GLOBAL_REVISION, &kv_lsn));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    *lsn = kv_lsn > 0 ? kv_lsn - 1 : 0;

    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

/*
 * palm_get_open_checkpoint --
 *     Get the currently open checkpoint id.
 */
static int
palm_get_open_checkpoint(WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t *checkpoint_id)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    uint64_t kv_checkpoint;
    int ret;

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, true));
    PALM_KV_ERR(palm, session,
      palm_kv_get_global(&context, PALM_KV_GLOBAL_CHECKPOINT_STARTED, &kv_checkpoint));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    *checkpoint_id = kv_checkpoint;

    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

static int
palm_handle_put(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_PUT_ARGS *put_args, const WT_ITEM *buf)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    PALM_HANDLE *palm_handle;
    uint64_t lsn;
    int ret;
    bool is_delta;

    is_delta = (put_args->flags & WT_PAGE_LOG_DELTA) != 0;
    lsn = 0;
    palm_handle = (PALM_HANDLE *)plh;
    palm = palm_handle->palm;
    palm_delay(palm);

    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_REVISION, &lsn);
    if (ret == MDB_NOTFOUND) {
        lsn = 1;
        ret = 0;
    }
    PALM_KV_ERR(palm, session, ret);

    PALM_VERBOSE_PRINT(palm_handle->palm,
      "palm_handle_put(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ", checkpoint_id=%" PRIu64 ", backlink_lsn=%" PRIu64 ", base_lsn=%" PRIu64
      ", backlink_checkpoint_id=%" PRIu64 ", base_checkpoint_id=%" PRIu64
      ", is_delta=%d, buf=\n%s)\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, checkpoint_id, put_args->backlink_lsn,
      put_args->base_lsn, put_args->backlink_checkpoint_id, put_args->base_checkpoint_id, is_delta,
      palm_verbose_item(buf));

    PALM_KV_ERR(palm, session,
      palm_kv_put_page(&context, palm_handle->table_id, page_id, lsn, checkpoint_id, is_delta,
        put_args->backlink_lsn, put_args->base_lsn, put_args->backlink_checkpoint_id,
        put_args->base_checkpoint_id, put_args->flags, buf));
    PALM_KV_ERR(palm, session, palm_kv_put_global(&context, PALM_KV_GLOBAL_REVISION, lsn + 1));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));
    put_args->lsn = lsn;
    return (0);

err:
    palm_kv_rollback_transaction(&context);

    PALM_VERBOSE_PRINT(palm_handle->palm,
      "palm_handle_put(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ", checkpoint_id=%" PRIu64 ", is_delta=%d) returned %d\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, checkpoint_id, is_delta, ret);
    return (ret);
}

#define PALM_GET_VERIFY_EQUAL(a, b)                                                                \
    {                                                                                              \
        if ((a) != (b)) {                                                                          \
            ret = palm_kv_err(palm, session, EINVAL,                                               \
              "%s:%d: Delta chain validation failed at position %" PRIu32                          \
              ": %s != %s. Page details: table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64  \
              ", %s=%" PRIu64 ", %s=%" PRIu64,                                                     \
              __func__, __LINE__, count, #a, #b, palm_handle->table_id, page_id, lsn, #a, (a), #b, \
              (b));                                                                                \
            goto err;                                                                              \
        }                                                                                          \
    }

static int
palm_handle_get(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_GET_ARGS *get_args, WT_ITEM *results_array,
  uint32_t *results_count)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    PALM_HANDLE *palm_handle;
    PALM_KV_PAGE_MATCHES matches;
    uint32_t count, i;
    uint64_t last_checkpoint_id, last_lsn, lsn;
    int ret;

    count = 0;
    last_checkpoint_id = 0;
    last_lsn = 0;
    lsn = get_args->lsn;
    palm_handle = (PALM_HANDLE *)plh;
    palm = palm_handle->palm;
    palm_delay(palm);

    /* Ensure that regular shared tables use LSNs. */
    assert(palm_handle->table_id == 1 || lsn > 0);

    palm_init_context(palm, &context);

    PALM_VERBOSE_PRINT(palm_handle->palm,
      "palm_handle_get(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ", checkpoint_id=%" PRIu64 ")...\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, checkpoint_id);
    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    PALM_KV_ERR(palm, session,
      palm_kv_get_page_matches(
        &context, palm_handle->table_id, page_id, lsn, checkpoint_id, &matches));
    for (count = 0; count < *results_count; ++count) {
        if (!palm_kv_next_page_match(&matches))
            break;
        memset(&results_array[count], 0, sizeof(WT_ITEM));
        PALM_KV_ERR(palm, session, palm_resize_item(&results_array[count], matches.size));
        memcpy(results_array[count].mem, matches.data, matches.size);

        /* Validate back links. */
        if (count > 0) {
            PALM_GET_VERIFY_EQUAL(matches.backlink_lsn, last_lsn);
            PALM_GET_VERIFY_EQUAL(matches.backlink_checkpoint_id, last_checkpoint_id);
        }

        /* Validate base. */
        if (count == 1) {
            PALM_GET_VERIFY_EQUAL(matches.base_lsn, last_lsn);
            PALM_GET_VERIFY_EQUAL(matches.base_checkpoint_id, last_checkpoint_id);
        } else if (count > 1) {
            PALM_GET_VERIFY_EQUAL(matches.base_lsn, get_args->base_lsn);
            PALM_GET_VERIFY_EQUAL(matches.base_checkpoint_id, get_args->base_checkpoint_id);
        }

        last_lsn = matches.lsn;
        last_checkpoint_id = matches.checkpoint_id;
        get_args->backlink_lsn = matches.backlink_lsn;
        get_args->base_lsn = matches.base_lsn;
        get_args->backlink_checkpoint_id = matches.backlink_checkpoint_id;
        get_args->base_checkpoint_id = matches.base_checkpoint_id;
        get_args->delta_count = count;
    }
    /* Did the caller give us enough output entries to hold all the results? */
    if (count == *results_count && palm_kv_next_page_match(&matches))
        PALM_KV_ERR(palm, session, ENOMEM);

    *results_count = count;
    PALM_KV_ERR(palm, session, matches.error);

err:
    palm_kv_rollback_transaction(&context);
    PALM_VERBOSE_PRINT(palm_handle->palm,
      "palm_handle_get(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ", checkpoint_id=%" PRIu64 ") returns %d (in %d parts)\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, checkpoint_id, ret, (int)count);
    if (ret == 0) {
        for (i = 0; i < count; ++i)
            PALM_VERBOSE_PRINT(
              palm_handle->palm, "   part %d: %s\n", (int)i, palm_verbose_item(&results_array[i]));
        PALM_VERBOSE_PRINT(palm_handle->palm,
          "   metadata: backlink_lsn=%" PRIu64 ", base_lsn=%" PRIu64
          ", backlink_checkpoint_id=%" PRIu64 ", base_checkpoint_id=%" PRIu64 "\n",
          get_args->backlink_lsn, get_args->base_lsn, get_args->backlink_checkpoint_id,
          get_args->base_checkpoint_id);
    }
    return (ret);
}

/*
 * palm_handle_close_internal --
 *     Internal file handle close.
 */
static int
palm_handle_close_internal(PALM *palm, PALM_HANDLE *palm_handle)
{
    int ret;
    WT_PAGE_LOG_HANDLE *plh;

    ret = 0;
    plh = (WT_PAGE_LOG_HANDLE *)palm_handle;

    (void)palm;
    (void)plh;
    /* TODO: placeholder for more actions */

    free(palm_handle);

    return (ret);
}

static int
palm_handle_close(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session)
{
    PALM_HANDLE *palm_handle;

    (void)session;

    palm_handle = (PALM_HANDLE *)plh;
    return (palm_handle_close_internal(palm_handle->palm, palm_handle));
}

/*
 * palm_open_handle --
 *     Open a handle for further operations on a table.
 */
static int
palm_open_handle(
  WT_PAGE_LOG *page_log, WT_SESSION *session, uint64_t table_id, WT_PAGE_LOG_HANDLE **plh)
{
    PALM *palm;
    PALM_HANDLE *palm_handle;

    (void)session;

    palm = (PALM *)page_log;
    if ((palm_handle = calloc(1, sizeof(PALM_HANDLE))) == NULL)
        return (errno);
    palm_handle->iface.page_log = page_log;
    palm_handle->iface.plh_put = palm_handle_put;
    palm_handle->iface.plh_get = palm_handle_get;
    palm_handle->iface.plh_close = palm_handle_close;
    palm_handle->palm = palm;
    palm_handle->table_id = table_id;

    *plh = &palm_handle->iface;

    return (0);
}

/*
 * palm_set_last_materialized_lsn --
 *     Set the last materialized LSN for testing purposes.
 */
static int
palm_set_last_materialized_lsn(WT_PAGE_LOG *storage, WT_SESSION *session, uint64_t lsn)
{
    PALM *palm;

    (void)session;

    palm = (PALM *)storage;
    palm->last_materialized_lsn = lsn;

    return (0);
}

/*
 * palm_terminate --
 *     Discard any resources on termination
 */
static int
palm_terminate(WT_PAGE_LOG *storage, WT_SESSION *session)
{
    PALM_HANDLE *palm_handle, *safe_handle;
    PALM *palm;
    int ret;

    ret = 0;
    palm = (PALM *)storage;

    if (--palm->reference_count != 0)
        return (0);

    /*
     * We should be single threaded at this point, so it is safe to destroy the lock and access the
     * file handle list without locking it.
     */
    if ((ret = pthread_rwlock_destroy(&palm->pl_handle_lock)) != 0)
        (void)palm_err(palm, session, ret, "terminate: pthread_rwlock_destroy");

    TAILQ_FOREACH_SAFE(palm_handle, &palm->fileq, q, safe_handle)
    palm_handle_close_internal(palm, palm_handle);

    if (palm->kv_env != NULL)
        palm_kv_env_close(palm->kv_env);
    if (palm->kv_home != NULL)
        free(palm->kv_home);
    free(palm);

    return (ret);
}

int palm_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config);

/*
 * palm_extension_init --
 *     A standalone, durable implementation of the WT_PAGE_LOG interface (PALI).
 */
int
palm_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    PALM *palm;
    const char *home;
    size_t len;
    int ret;

    if ((palm = calloc(1, sizeof(PALM))) == NULL)
        return (errno);
    palm->wt_api = connection->get_extension_api(connection);
    if ((ret = pthread_rwlock_init(&palm->pl_handle_lock, NULL)) != 0) {
        (void)palm_err(palm, NULL, ret, "pthread_rwlock_init");
        free(palm);
        return (ret);
    }

    /*
     * Allocate a palm storage structure, with a WT_STORAGE structure as the first field, allowing
     * us to treat references to either type of structure as a reference to the other type.
     */
    palm->page_log.pl_add_reference = palm_add_reference;
    palm->page_log.pl_begin_checkpoint = palm_begin_checkpoint;
    palm->page_log.pl_complete_checkpoint = palm_complete_checkpoint;
    palm->page_log.pl_complete_checkpoint_ext = palm_complete_checkpoint_ext;
    palm->page_log.pl_get_complete_checkpoint = palm_get_complete_checkpoint;
    palm->page_log.pl_get_complete_checkpoint_ext = palm_get_complete_checkpoint_ext;
    palm->page_log.pl_get_last_lsn = palm_get_last_lsn;
    palm->page_log.pl_get_open_checkpoint = palm_get_open_checkpoint;
    palm->page_log.pl_open_handle = palm_open_handle;
    palm->page_log.pl_set_last_materialized_lsn = palm_set_last_materialized_lsn;
    palm->page_log.terminate = palm_terminate;

    /*
     * The first reference is implied by the call to add_page_log.
     */
    palm->reference_count = 1;

    if ((ret = palm_configure(palm, config)) != 0)
        goto err;

    /* Load the storage */
    PALM_KV_ERR(palm, NULL, connection->add_page_log(connection, "palm", &palm->page_log, NULL));
    PALM_KV_ERR(palm, NULL, palm_kv_env_create(&palm->kv_env, palm->cache_size_mb));

    /* Build the LMDB home string. */
    home = connection->get_home(connection);
    len = strlen(home) + 20;
    palm->kv_home = malloc(len);
    if (palm->kv_home == NULL) {
        ret = palm_err(palm, NULL, errno, "malloc");
        goto err;
    }
    strncpy(palm->kv_home, home, len);
    strncat(palm->kv_home, "/kv_home", len);

    /* Create the LMDB home, or if it exists, use what is already there. */
    ret = mkdir(palm->kv_home, 0777);
    if (ret != 0) {
        ret = errno;
        if (ret == EEXIST)
            ret = 0;
        else {
            ret = palm_err(palm, NULL, ret, "mkdir");
            goto err;
        }
    }

    /* Open the LMDB environment. */
    PALM_KV_ERR(palm, NULL, palm_kv_env_open(palm->kv_env, palm->kv_home));

err:
    if (ret != 0) {
        if (palm->kv_env != NULL)
            palm_kv_env_close(palm->kv_env);
        if (palm->kv_home != NULL)
            free(palm->kv_home);
        free(palm);
    }
    return (ret);
}

/*
 * We have to remove this symbol when building as a builtin extension otherwise it will conflict
 * with other builtin libraries.
 */
#ifndef HAVE_BUILTIN_EXTENSION_PALM
/*
 * wiredtiger_extension_init --
 *     WiredTiger page and log mock extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
    return palm_extension_init(connection, config);
}
#endif
