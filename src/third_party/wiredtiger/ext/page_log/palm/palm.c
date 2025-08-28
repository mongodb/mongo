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
#include "palm_utils.h"

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

#define PALM_ENCRYPTION_EQUAL(e1, e2) (memcmp((e1).dek, (e2).dek, sizeof((e1).dek)) == 0)
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

    /* The LSN when the KV database is opened, used to check encryption. */
    uint64_t begin_lsn;

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
    bool verbose_msg;                  /* Send verbose messages to msg callback interface */
    bool verify;                       /* Perform verification throughout the workload. */

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
static int palm_configure_bool(PALM *, WT_CONFIG_PARSER *, WT_CONFIG_ARG *, const char *, bool *);
static int palm_configure_int(
  PALM *, WT_CONFIG_PARSER *, WT_CONFIG_ARG *, const char *, uint32_t *);
static int palm_err(PALM *, WT_SESSION *, int, const char *, ...);
static int palm_kv_err(PALM *, WT_SESSION *, int, const char *, ...);
static int palm_get_dek(PALM *, WT_SESSION *, const WT_PAGE_LOG_ENCRYPTION *, uint64_t, uint64_t,
  bool, uint64_t, WT_PAGE_LOG_ENCRYPTION *);
static void palm_init_context(PALM *, PALM_KV_CONTEXT *);
static int palm_init_lsn(PALM *);

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
    if ((ret = palm_configure_bool(palm, env_parser, config, "verbose_msg", &palm->verbose_msg)) !=
      0)
        goto err;
    if ((ret = palm_configure_bool(palm, env_parser, config, "verify", &palm->verify)) != 0)
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
 * palm_configure_bool
 *     Look for a particular configuration key, and return its boolean value.
 */
static int
palm_configure_bool(
  PALM *palm, WT_CONFIG_PARSER *env_parser, WT_CONFIG_ARG *config, const char *key, bool *valuep)
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
        if (v.len == 0 || (v.type != WT_CONFIG_ITEM_NUM && v.type != WT_CONFIG_ITEM_BOOL))
            ret = palm_err(palm, NULL, EINVAL, "force_error config arg: bool required");
        else
            *valuep = (v.val != 0);
    } else if (ret == WT_NOTFOUND)
        ret = 0;
    else
        ret = palm_err(palm, NULL, EINVAL, "WT_API->config_get");

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
palm_delay(PALM *palm, WT_SESSION *session)
{
    int ret;
    uint64_t us;

    ret = 0;
    if (palm->force_delay != 0 &&
      (palm->object_gets + palm->object_puts) % palm->force_delay == 0) {
        us = palm_compute_delay_us(palm, (uint64_t)palm->delay_ms * WT_THOUSAND);
        PALM_VERBOSE_PRINT(palm, session,
          "Artificial delay %" PRIu64 " microseconds after %" PRIu64 " object reads, %" PRIu64
          " object writes\n",
          us, palm->object_gets, palm->object_puts);
        sleep_us(us);
    }
    if (palm->force_error != 0 &&
      (palm->object_gets + palm->object_puts) % palm->force_error == 0) {
        us = palm_compute_delay_us(palm, (uint64_t)palm->error_ms * WT_THOUSAND);
        PALM_VERBOSE_PRINT(palm, session,
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
    PALM_VERBOSE_PRINT(palm, session, "palm LMDB: %s: %s\n", lmdb_error, buf);
    va_end(ap);

    return (WT_ERROR);
}

/*
 * palm_get_dek --
 *     Check or generate a DEK (encryption key).
 */
static int
palm_get_dek(PALM *palm, WT_SESSION *session, const WT_PAGE_LOG_ENCRYPTION *encrypt_in,
  uint64_t table_id, uint64_t page_id, bool is_delta, uint64_t base_lsn,
  WT_PAGE_LOG_ENCRYPTION *encrypt_out)
{
    static WT_PAGE_LOG_ENCRYPTION zero_encryption;
    WT_PAGE_LOG_ENCRYPTION tmp;
    bool was_zeroed;

    /*
     * The DEK is an encrypted encryption key. A production implementation of the page log interface
     * would do encryption, using the DEK when it is set. If the DEK is not set, the implementation
     * must figure out what the DEK should be, which may take some time. The DEK is stored with the
     * page, and when the implementation gets a page it knows how to decrypt it. It also passes the
     * DEK to the user of the interface (WiredTiger). That DEK must be kept and used for subsequent
     * deltas to the page. Thus when deltas are written, the DEK doesn't have to be recomputed.
     *
     * Here in PALM, we don't want to do any encryption. Since the encrypt/decryption would
     * invisible to the calling layer (WiredTiger), having encryption doesn't help test WiredTiger
     * at all. Also, it gets in the way of efficient debugging.
     *
     * However, we do want to test that WiredTiger is passing along the DEK whenever it can and
     * should. If it stopped doing so, the production page log would need to determine the DEK for
     * itself more often, and we might not notice the error.
     *
     * So WiredTiger receives a DEK with every page get. When writing a delta for such a page, it
     * needs to pass that DEK. One the other hand, when writing a delta for page that WiredTiger
     * generated and wrote during the current connection, it uses a zeroed DEK, that's the best it
     * can do.
     *
     * To test this without doing any extra KV requests, we generate and store a DEK for any page
     * write that doesn't already have it - a simple encoding of the table id and page id. Then,
     * we'd expect that if a DEK is ever passed to us in the put path, it must match that simple
     * encoding. That tests that the correct DEK is being passed.
     *
     * To test that we're passing a DEK when we should, we compare the base_lsn to the LSN we
     * started the run with. If the base_lsn is less than that, then WiredTiger must have previously
     * gotten the page from the page log interface, hence the DEK should be set.
     */
#define PALM_DEK_FORMAT ("%" PRIu64 ":%" PRIu64)
    tmp = zero_encryption;
    if ((size_t)snprintf(&tmp.dek[0], sizeof(tmp.dek), PALM_DEK_FORMAT, table_id, page_id) >
      sizeof(tmp.dek))
        assert(false); /* should never overflow */

    was_zeroed = PALM_ENCRYPTION_EQUAL(*encrypt_in, zero_encryption);
    if (was_zeroed)
        *encrypt_out = tmp;
    else {
        if (!PALM_ENCRYPTION_EQUAL(*encrypt_in, tmp))
            return (palm_err(palm, session, EINVAL,
              "encryption dek %31s does not match expected value %31s", encrypt_in->dek, tmp.dek));
        PALM_VERBOSE_PRINT(palm, session, "palm using saved dek: %s\n", encrypt_in->dek);
        *encrypt_out = *encrypt_in;
    }

    if (was_zeroed && is_delta && base_lsn < palm->begin_lsn)
        return (palm_err(palm, session, EINVAL, "expected non-zero encryption dek"));

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
 * palm_init_lsn --
 *     Remember the current LSN when we started PALM.
 */
static int
palm_init_lsn(PALM *palm)
{
    PALM_KV_CONTEXT context;
    int ret;

    palm_init_context(palm, &context);
    PALM_KV_RET(palm, NULL, palm_kv_begin_transaction(&context, palm->kv_env, false));

    /*
     * Get the LSN. If it's never been set, we'll get not found, but that's okay, that will leave
     * our beginning LSN at zero, which is fine for our purposes.
     */
    ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_LSN, &palm->begin_lsn);
    if (ret == MDB_NOTFOUND)
        ret = 0;
    palm_kv_rollback_transaction(&context);
    return (ret);
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
    int ret;
    ret = 0;

    (void)page_log;      /* Unused parameter */
    (void)session;       /* Unused parameter */
    (void)checkpoint_id; /* Unused parameter */

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
    uint64_t lsn;
    int ret;

    (void)checkpoint_id; /* Unused parameter */

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_LSN, &lsn);
    if (ret == MDB_NOTFOUND) {
        lsn = 1;
        ret = 0;
    }
    PALM_KV_ERR(palm, session, ret);
    PALM_KV_ERR(palm, session,
      palm_kv_put_checkpoint(&context, lsn, checkpoint_timestamp, checkpoint_metadata));
    PALM_KV_ERR(palm, session, palm_kv_put_global(&context, PALM_KV_GLOBAL_LSN, lsn + 1));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    if (lsnp != NULL)
        *lsnp = lsn;
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

    (void)checkpoint_id; /* Unused parameter */

    metadata = NULL;
    metadata_len = 0;
    if (checkpoint_lsn != NULL)
        *checkpoint_lsn = 0;
    if (checkpoint_timestamp != NULL)
        *checkpoint_timestamp = 0;
    if (checkpoint_metadata != NULL)
        memset(checkpoint_metadata, 0, sizeof(WT_ITEM));

    palm = (PALM *)page_log;
    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, true));

    ret = palm_kv_get_last_checkpoint(
      &context, checkpoint_lsn, checkpoint_timestamp, &metadata, &metadata_len);
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
    PALM_KV_ERR(palm, session, palm_kv_get_global(&context, PALM_KV_GLOBAL_LSN, &kv_lsn));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    *lsn = kv_lsn > 0 ? kv_lsn - 1 : 0;

    return (0);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

#define PALM_VERIFY_EQUAL(a, b)                                                                   \
    {                                                                                             \
        if ((a) != (b)) {                                                                         \
            ret = palm_kv_err(palm, session, EINVAL,                                              \
              "%s:%d: Delta chain validation failed at position %" PRIu32                         \
              ": %s != %s. Page details: table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64 \
              ", flags=%" PRIx64 ", %s=%" PRIu64 ", %s=%" PRIu64,                                 \
              __func__, __LINE__, count, #a, #b, palm_handle->table_id, page_id, matches.lsn,     \
              matches.flags, #a, (a), #b, (b));                                                   \
            goto err;                                                                             \
        }                                                                                         \
    }

static int
palm_handle_verify_page(
  WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session, uint64_t page_id, uint64_t lsn)
{
    PALM *palm;
    PALM_KV_CONTEXT context;
    PALM_HANDLE *palm_handle;
    PALM_KV_PAGE_MATCHES matches;
    uint32_t count;
    uint64_t last_base_lsn, last_lsn;
    bool last_tombstone;
    int ret;

    count = 0;
    last_base_lsn = last_lsn = 0;
    last_tombstone = false;
    palm_handle = (PALM_HANDLE *)plh;
    palm = palm_handle->palm;

    palm_init_context(palm, &context);
    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    PALM_KV_ERR(palm, session,
      palm_kv_get_page_matches(&context, palm_handle->table_id, page_id, lsn, &matches));
    while (palm_kv_next_page_match(&matches)) {

        /* FIXME-WT-15041: Enable the following once PALM can handle abandoned checkpoints. */
        (void)last_tombstone;
#if 0
        /* Only the last page in the chain can be a tombstone. */
        PALM_VERIFY_EQUAL(last_tombstone, false);

        /* Validate backlink LSN. */
        if (count > 0)
            PALM_VERIFY_EQUAL(matches.backlink_lsn, last_lsn);
#endif

        /* Validate base LSN. */
        if (count == 1) {
            PALM_VERIFY_EQUAL(matches.base_lsn, last_lsn);
        } else if (count > 1) {
            PALM_VERIFY_EQUAL(matches.base_lsn, last_base_lsn);
        }

        count++;
        last_base_lsn = matches.base_lsn;
        last_lsn = matches.lsn;
        if ((matches.flags & WT_PALM_KV_TOMBSTONE) != 0)
            last_tombstone = true;
    }
    PALM_KV_ERR(palm, session, matches.error);

err:
    palm_kv_rollback_transaction(&context);
    return (ret);
}

static int
palm_handle_discard(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_DISCARD_ARGS *discard_args)
{
    static WT_PAGE_LOG_ENCRYPTION zero_encryption;
    WT_ITEM *tombstone = NULL;
    PALM_HANDLE *palm_handle = (PALM_HANDLE *)plh;
    PALM *palm = palm_handle->palm;
    palm_delay(palm, session);

    PALM_KV_CONTEXT context;
    palm_init_context(palm, &context);

    (void)checkpoint_id; /* Unused parameter */

    /* Tombstones are deltas. */
    bool is_delta = true;
    uint32_t flags = WT_PALM_KV_TOMBSTONE | WT_PAGE_LOG_DELTA;

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    uint64_t lsn;
    int ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_LSN, &lsn);
    if (ret == MDB_NOTFOUND) {
        lsn = 1;
        ret = 0;
    }
    PALM_KV_ERR(palm, session, ret);

    PALM_VERBOSE_PRINT(palm_handle->palm, session,
      "palm_handle_discard(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", backlink_lsn=%" PRIu64
      ", base_lsn=%" PRIu64 ")\n",
      (void *)plh, palm_handle->table_id, page_id, discard_args->backlink_lsn,
      discard_args->base_lsn);

    /* There should not be any flag set. */
    assert(discard_args->flags == 0);

    /* Create an empty record as a tombstone. */
    if ((tombstone = calloc(1, sizeof(WT_ITEM))) == NULL)
        return (errno);

    PALM_KV_ERR(palm, session,
      palm_kv_put_page(&context, palm_handle->table_id, page_id, lsn, is_delta,
        discard_args->backlink_lsn, discard_args->base_lsn, &zero_encryption, flags, tombstone));
    PALM_KV_ERR(palm, session, palm_kv_put_global(&context, PALM_KV_GLOBAL_LSN, lsn + 1));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));

    discard_args->lsn = lsn;

    /* Verify the delta chain. */
    if (palm->verify)
        PALM_KV_ERR(palm, session, palm_handle_verify_page(plh, session, page_id, lsn));

    if (0) {
err:
        palm_kv_rollback_transaction(&context);

        PALM_VERBOSE_PRINT(palm_handle->palm, session,
          "palm_handle_discard(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
          ", is_delta=%d) returned %d\n",
          (void *)plh, palm_handle->table_id, page_id, lsn, is_delta, ret);
    }

    free(tombstone);

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
    WT_PAGE_LOG_ENCRYPTION encryption;

    (void)checkpoint_id; /* Unused parameter */

    is_delta = (put_args->flags & WT_PAGE_LOG_DELTA) != 0;
    lsn = 0;
    palm_handle = (PALM_HANDLE *)plh;
    palm = palm_handle->palm;
    palm_delay(palm, session);

    palm_init_context(palm, &context);

    /* Check or initialize the encryption field. */
    PALM_KV_RET(palm, session,
      palm_get_dek(palm, session, &put_args->encryption, palm_handle->table_id, page_id, is_delta,
        put_args->base_lsn, &encryption));

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    ret = palm_kv_get_global(&context, PALM_KV_GLOBAL_LSN, &lsn);
    if (ret == MDB_NOTFOUND) {
        lsn = 1;
        ret = 0;
    }
    PALM_KV_ERR(palm, session, ret);

    PALM_VERBOSE_PRINT(palm_handle->palm, session,
      "palm_handle_put(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ", backlink_lsn=%" PRIu64 ", base_lsn=%" PRIu64 ", is_delta=%d, buf=\n%s)\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, put_args->backlink_lsn, put_args->base_lsn,
      is_delta, palm_verbose_item(buf));

    PALM_KV_ERR(palm, session,
      palm_kv_put_page(&context, palm_handle->table_id, page_id, lsn, is_delta,
        put_args->backlink_lsn, put_args->base_lsn, &encryption, put_args->flags, buf));
    PALM_KV_ERR(palm, session, palm_kv_put_global(&context, PALM_KV_GLOBAL_LSN, lsn + 1));
    PALM_KV_ERR(palm, session, palm_kv_commit_transaction(&context));
    put_args->lsn = lsn;
    return (0);

err:
    palm_kv_rollback_transaction(&context);

    PALM_VERBOSE_PRINT(palm_handle->palm, session,
      "palm_handle_put(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ", is_delta=%d) returned %d\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, is_delta, ret);
    return (ret);
}

static int
palm_handle_get_page_ids(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session, uint64_t checkpoint_lsn,
  uint64_t table_id, WT_ITEM *item, size_t *size)
{
    PALM_KV_CONTEXT context;
    PALM_HANDLE *palm_handle = (PALM_HANDLE *)plh;
    PALM *palm = palm_handle->palm;

    palm_init_context(palm, &context);

    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));

    int ret;
    ret = palm_kv_get_page_ids(&context, item, checkpoint_lsn, table_id, size);

    palm_kv_rollback_transaction(&context);

    return (ret);
}

static int
palm_handle_get(WT_PAGE_LOG_HANDLE *plh, WT_SESSION *session, uint64_t page_id,
  uint64_t checkpoint_id, WT_PAGE_LOG_GET_ARGS *get_args, WT_ITEM *results_array,
  uint32_t *results_count)
{
    static WT_PAGE_LOG_ENCRYPTION zero_encryption;
    PALM *palm;
    PALM_KV_CONTEXT context;
    PALM_HANDLE *palm_handle;
    PALM_KV_PAGE_MATCHES matches;
    uint32_t count, i;
    uint64_t last_lsn, lsn;
    int ret;
    bool zeroed_encryption, was_zeroed_encryption;

    (void)checkpoint_id; /* Unused parameter */

    count = 0;
    last_lsn = 0;
    lsn = get_args->lsn;
    palm_handle = (PALM_HANDLE *)plh;
    palm = palm_handle->palm;
    palm_delay(palm, session);

    /* Ensure that regular shared tables use LSNs. */
    assert(palm_handle->table_id == 1 || lsn > 0);

    palm_init_context(palm, &context);

    PALM_VERBOSE_PRINT(palm_handle->palm, session,
      "palm_handle_get(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64 ")...\n",
      (void *)plh, palm_handle->table_id, page_id, lsn);
    PALM_KV_RET(palm, session, palm_kv_begin_transaction(&context, palm->kv_env, false));
    PALM_KV_ERR(palm, session,
      palm_kv_get_page_matches(&context, palm_handle->table_id, page_id, lsn, &matches));
    get_args->encryption = zero_encryption;
    was_zeroed_encryption = true;
    for (count = 0; count < *results_count; ++count) {
        if (!palm_kv_next_page_match(&matches))
            break;
        memset(&results_array[count], 0, sizeof(WT_ITEM));
        PALM_KV_ERR(palm, session, palm_resize_item(&results_array[count], matches.size));
        memcpy(results_array[count].mem, matches.data, matches.size);

        if (palm->verify) {
            /* Validate backlink LSN. */
            if (count > 0)
                PALM_VERIFY_EQUAL(matches.backlink_lsn, last_lsn);

            /* Validate base LSN. */
            if (count == 1) {
                PALM_VERIFY_EQUAL(matches.base_lsn, last_lsn);
            } else if (count > 1) {
                PALM_VERIFY_EQUAL(matches.base_lsn, get_args->base_lsn);
            }
        }

        /* We should not request a page that is discarded. */
        ret = (matches.flags & WT_PALM_KV_TOMBSTONE) == 0 ? 0 : EINVAL;
        PALM_KV_ERR(palm, session, ret);

        last_lsn = matches.lsn;
        get_args->backlink_lsn = matches.backlink_lsn;
        get_args->base_lsn = matches.base_lsn;
        get_args->encryption = matches.encryption;
        zeroed_encryption = PALM_ENCRYPTION_EQUAL(get_args->encryption, zero_encryption);
        if (zeroed_encryption)
            PALM_VERBOSE_PRINT(palm, session, "palm got zero dek%s\n", "");
        else
            PALM_VERBOSE_PRINT(
              palm, session, "palm got non-zero dek: %s\n", get_args->encryption.dek);
        if (zeroed_encryption && !was_zeroed_encryption) {
            ret = palm_err(palm, session, EINVAL,
              "base dek is not zeroed, delta encryption is zero and should not be");
            goto err;
        }
    }
    /* Did the caller give us enough output entries to hold all the results? */
    if (count == *results_count && palm_kv_next_page_match(&matches))
        PALM_KV_ERR(palm, session, ENOMEM);

    *results_count = count;
    PALM_KV_ERR(palm, session, matches.error);

err:
    palm_kv_rollback_transaction(&context);
    PALM_VERBOSE_PRINT(palm_handle->palm, session,
      "palm_handle_get(plh=%p, table_id=%" PRIu64 ", page_id=%" PRIu64 ", lsn=%" PRIu64
      ") returns %d (in %d parts)\n",
      (void *)plh, palm_handle->table_id, page_id, lsn, ret, (int)count);
    if (ret == 0) {
        for (i = 0; i < count; ++i)
            PALM_VERBOSE_PRINT(palm_handle->palm, session, "   part %d: %s\n", (int)i,
              palm_verbose_item(&results_array[i]));
        PALM_VERBOSE_PRINT(palm_handle->palm, session,
          "   metadata: backlink_lsn=%" PRIu64 ", base_lsn=%" PRIu64 "\n", get_args->backlink_lsn,
          get_args->base_lsn);
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
    palm_handle->iface.plh_discard = palm_handle_discard;
    palm_handle->iface.plh_put = palm_handle_put;
    palm_handle->iface.plh_get = palm_handle_get;
    palm_handle->iface.plh_get_page_ids = palm_handle_get_page_ids;
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
    palm->page_log.pl_complete_checkpoint = NULL;
    palm->page_log.pl_complete_checkpoint_ext = palm_complete_checkpoint_ext;
    palm->page_log.pl_get_complete_checkpoint = NULL;
    palm->page_log.pl_get_complete_checkpoint_ext = palm_get_complete_checkpoint_ext;
    palm->page_log.pl_get_last_lsn = palm_get_last_lsn;
    palm->page_log.pl_get_open_checkpoint = NULL;
    palm->page_log.pl_open_handle = palm_open_handle;
    palm->page_log.pl_set_last_materialized_lsn = palm_set_last_materialized_lsn;
    palm->page_log.terminate = palm_terminate;

    /*
     * The first reference is implied by the call to add_page_log.
     */
    palm->reference_count = 1;

    /* Turn on verification by default, as PALM is used primarily for testing. */
    palm->verify = true;

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

    if ((ret = palm_init_lsn(palm)) != 0)
        goto err;

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
