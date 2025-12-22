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

#include "key_provider.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Format specifier for size_t */
#if defined(_MSC_VER) && _MSC_VER < 1900
#define PRIzu "Iu" /* size_t format string for MSVC before VS2015 */
#else
#define PRIzu "zu" /* size_t format string */
#endif

/* Logging macros */
#define LOG_AT(kp, session, level, fmt, ...)                                                   \
    do {                                                                                       \
        if ((kp)->verbose >= (level)) {                                                        \
            ((level) == WT_VERBOSE_ERROR ? (kp)->wtext->err_printf : (kp)->wtext->msg_printf)( \
              (kp)->wtext, (session), "%p, %s: " fmt, (void *)(kp), __func__, ##__VA_ARGS__);  \
        }                                                                                      \
    } while (0)

#define LOG_INFO(kp, session, ...) LOG_AT((kp), (session), WT_VERBOSE_INFO, __VA_ARGS__)
#define LOG_DEBUG(kp, session, ...) LOG_AT((kp), (session), WT_VERBOSE_DEBUG_1, __VA_ARGS__)
#define LOG_ERROR(kp, session, ...) LOG_AT((kp), (session), WT_VERBOSE_ERROR, __VA_ARGS__)

/* Convert clock ticks to seconds */
#define CLOCK_SECS(ct) ((double)(ct) / CLOCKS_PER_SEC)

/* Date format for ISO 8601 */
#define DATE_FORMAT_ISO8601 "%Y-%m-%dT%H:%M:%S%z"

/* Default initial key */
static const char DEFAULT_KEY_DATA[] = "abcdefghijklmnopqrstuvwxyz";

static const WT_CRYPT_KEYS DEFAULT_KEY = {
  .r = {.lsn = 0},
  .keys = {.data = (const void *)DEFAULT_KEY_DATA, .size = sizeof(DEFAULT_KEY_DATA) - 1},
};

/*
 * kp_free_key --
 *     Free the current key stored in the key provider.
 */
static void
kp_free_key(KEY_PROVIDER *kp)
{
    free(kp->state.key_data);
    memset(&kp->state, 0, sizeof(kp->state));
}

/*
 * kp_set_key --
 *     Set a new current key in the key provider.
 */
static int
kp_set_key(KEY_PROVIDER *kp, const WT_CRYPT_KEYS *crypt)
{
    const void *key_data = crypt->keys.data;
    size_t key_size = crypt->keys.size;
    uint64_t lsn = crypt->r.lsn;

    /* If no key data provided, use the default key */
    if (key_data == NULL || key_size == 0) {
        key_data = DEFAULT_KEY.keys.data;
        key_size = DEFAULT_KEY.keys.size;
        lsn = DEFAULT_KEY.r.lsn;
    }

    /* Verify that the key data matches the expected key data */
    assert(memcmp(key_data, DEFAULT_KEY_DATA, sizeof(DEFAULT_KEY_DATA) - 1) == 0);

    kp_free_key(kp);

    kp->state.key_data = malloc(key_size);
    if (kp->state.key_data == NULL)
        return (ENOMEM);

    memcpy(kp->state.key_data, key_data, key_size);
    kp->state.key_size = key_size;
    kp->state.lsn = lsn;

    kp->state.key_time = clock();
    kp->state.key_state = KEY_STATE_CURRENT;

    return (0);
}

/*
 * kp_load_key --
 *     Loads the current persisted key during checkpoint load. This is called by WiredTiger when
 *     loading a checkpoint to retrieve the key that was used when that checkpoint was created.
 */
static int
kp_load_key(WT_KEY_PROVIDER *wtkp, WT_SESSION *session, const WT_CRYPT_KEYS *crypt)
{
    KEY_PROVIDER *kp = (KEY_PROVIDER *)wtkp;
    LOG_DEBUG(kp, session, "Current key: LSN=%" PRIu64 ", key_time=%.2f, size=%" PRIzu,
      kp->state.lsn, CLOCK_SECS(kp->state.key_time), kp->state.key_size);

    LOG_INFO(
      kp, session, "Loading key for LSN=%" PRIu64 ", size=%" PRIzu, crypt->r.lsn, crypt->keys.size);

    assert(kp->state.key_state == KEY_STATE_CURRENT);
    kp_set_key(kp, crypt);

    return (0);
}

/*
 * kp_key_expired --
 *     Check if the current key has expired based on the configured expiration time.
 */
static bool
kp_key_expired(KEY_PROVIDER *kp)
{
    if (kp->key_expires == KEY_EXPIRES_NEVER)
        return (false); /* Key does not expire */
    else if (kp->key_expires == KEY_EXPIRES_ALWAYS)
        return (true); /* Key always expires */

    const clock_t now = clock();
    double elapsed_sec = CLOCK_SECS(now - kp->state.key_time);

    return (elapsed_sec >= kp->key_expires);
}

/*
 * kp_generate_key --
 *     Generate a new key with a repeating pattern.
 */
static int
kp_generate_key(uint8_t **new_key, size_t *key_size)
{
    /* Calculate new key size with 20% random fluctuation */
    const size_t base_size = 1024;
    const int fluctuation = (rand() % 41) - 20; /* -20% to +20% */
    const size_t new_key_size = base_size + (size_t)((int)base_size * fluctuation / 100);

    /* Allocate new key buffer */
    uint8_t *key_buf = malloc(new_key_size);
    if (key_buf == NULL)
        return (ENOMEM);

    /* Fill with repeating pattern: default key data + ISO8601 time */
    char iso_time[100] = {0};
    char pattern[sizeof(DEFAULT_KEY_DATA) + sizeof(iso_time)] = {0};
    strncat(pattern, DEFAULT_KEY_DATA, sizeof(pattern) - 1);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(iso_time, sizeof(iso_time), DATE_FORMAT_ISO8601, tm_info);
    strncat(pattern, iso_time, sizeof(pattern) - strlen(pattern) - 1);

    const size_t pattern_len = strlen(pattern);

    /* Fill buffer by repeatedly copying the pattern */
    size_t remaining = new_key_size;
    size_t offset = 0;
    while (remaining > 0) {
        const size_t copy_len = (remaining >= pattern_len) ? pattern_len : remaining;
        memcpy(key_buf + offset, pattern, copy_len);
        offset += copy_len;
        remaining -= copy_len;
    }

    *new_key = key_buf;
    *key_size = new_key_size;

    return (0);
}

/*
 * kp_rotate_key --
 *     Rotate the current key by generating a new key with a repeating pattern.
 */
static int
kp_rotate_key(KEY_PROVIDER *kp, WT_SESSION *session)
{
    int ret = 0;
    WT_CRYPT_KEYS crypt = {{0}, {0}};

    if ((ret = kp_generate_key((uint8_t **)&crypt.keys.data, &crypt.keys.size)) != 0) {
        LOG_ERROR(kp, session, "Failed to generate new key: %d", ret);
        return (ret);
    }

    if ((ret = kp_set_key(kp, &crypt)) != 0) {
        LOG_ERROR(kp, session, "Failed to set new key: %d", ret);
    }

    free((void *)crypt.keys.data);

    return (ret);
}

/*
 * kp_get_key --
 *     Fetch the latest key for checkpoint writes. The WT_CRYPT_KEYS::keys::size should be set to
 *     zero if the key has not changed. This is called by WiredTiger when creating a checkpoint to
 *     get the current encryption key.
 */
static int
kp_get_key(WT_KEY_PROVIDER *wtkp, WT_SESSION *session, WT_CRYPT_KEYS *crypt)
{
    KEY_PROVIDER *kp = (KEY_PROVIDER *)wtkp;
    LOG_DEBUG(kp, session, "Current key: LSN=%" PRIu64 ", key_time=%.2f, size=%" PRIzu,
      kp->state.lsn, CLOCK_SECS(kp->state.key_time), kp->state.key_size);

    /*
     * Real key provider may rotate the key independently of the get_key calls. In the mock
     * implementation the key is rotated only when its size has been requested. This is to prevent
     * key rotation between paired get_key calls: first requesting the size, then filling the data.
     */
    if (crypt->keys.data == NULL && kp_key_expired(kp)) {
        LOG_INFO(kp, session, "Key expired (key_time=%.2f)", CLOCK_SECS(kp->state.key_time));

        /* Key must be current */
        assert(kp->state.key_state == KEY_STATE_CURRENT);

        int ret = kp_rotate_key(kp, session);
        if (ret != 0) {
            LOG_ERROR(kp, session, "Failed to rotate key: %d", ret);
            return (ret);
        }

        LOG_INFO(kp, session, "Reporting new key (key_time=%.2f, key_size=%" PRIzu ")",
          CLOCK_SECS(kp->state.key_time), kp->state.key_size);
        crypt->keys.size = kp->state.key_size;
        kp->state.key_state = KEY_STATE_PENDING;
    } else if (crypt->keys.data != NULL) {
        /* Key must be pending read */
        assert(kp->state.key_state == KEY_STATE_PENDING);
        /* The size of requested data must match previously reported key size. */
        assert(crypt->keys.size == kp->state.key_size);

        LOG_INFO(kp, session, "Providing new key data (key_time=%.2f, key_size=%" PRIzu ")",
          CLOCK_SECS(kp->state.key_time), kp->state.key_size);
        memcpy((void *)crypt->keys.data, kp->state.key_data, crypt->keys.size);
        kp->state.key_state = KEY_STATE_READ;
    } else {
        LOG_INFO(kp, session, "Key is still valid, no change (key_time=%.2f)",
          CLOCK_SECS(kp->state.key_time));
        assert(kp->state.key_state == KEY_STATE_CURRENT);
        crypt->keys.size = 0;
    }

    return (0);
}

/*
 * kp_on_key_update --
 *     Callback function indicating whether the key has been queued. On success, the result field
 *     contains LSN of the checkpoint the key belongs to. On failure, the result field is set to the
 *     error code and the size is set to 0. This function can only be called after a successful
 *     get_key that returned new key data.
 */
static int
kp_on_key_update(WT_KEY_PROVIDER *wtkp, WT_SESSION *session, const WT_CRYPT_KEYS *crypt)
{
    KEY_PROVIDER *kp = (KEY_PROVIDER *)wtkp;
    LOG_DEBUG(kp, session, "Current key: LSN=%" PRIu64 ", key_time=%.2f, size=%" PRIzu,
      kp->state.lsn, CLOCK_SECS(kp->state.key_time), kp->state.key_size);

    assert(kp->state.key_data != NULL);
    assert(kp->state.key_state == KEY_STATE_READ); /* Key must have been read */

    if (crypt->keys.size == 0) {
        /* Failure case - error is in keys->r.error */
        LOG_ERROR(kp, session, "Key queueing failed with error %d", crypt->r.error);
        kp->state.lsn = 0; /* Reset LSN on failure */
    } else {
        /* Success case - LSN is in keys->r.lsn */
        LOG_INFO(kp, session, "Key queued successfully at LSN %" PRIu64, crypt->r.lsn);

        /* Update our internal state */
        assert(crypt->r.lsn != 0);
        kp->state.lsn = crypt->r.lsn;

        assert(memcmp(kp->state.key_data, crypt->keys.data, kp->state.key_size) == 0);
        assert(kp->state.key_size == crypt->keys.size);
        kp->state.key_state = KEY_STATE_CURRENT;
    }

    return (0);
}

/*
 * kp_terminate --
 *     Cleanup function called when the key provider is being shut down.
 */
static int
kp_terminate(WT_KEY_PROVIDER *wtkp, WT_SESSION *session)
{
    KEY_PROVIDER *kp = (KEY_PROVIDER *)wtkp;

    LOG_INFO(kp, session, "Terminating key provider");

    kp_free_key(kp);
    free(kp);

    return (0);
}

/* Configuration parsing helpers */

static int
configure_int(const char *param, const WT_CONFIG_ITEM *k, const WT_CONFIG_ITEM *v, int *dest)
{
    if (strncmp(param, k->str, k->len) == 0 && k->len == strlen(param) && v->len > 0 &&
      v->type == WT_CONFIG_ITEM_NUM) {
        *dest = (int)v->val;

        return (0);
    }

    return (EINVAL);
}

/*
 * kp_configure --
 *     Parse configuration options for the key provider.
 */
static int
kp_configure(KEY_PROVIDER *kp, WT_CONFIG_ARG *config)
{
    WT_EXTENSION_API *wtext = kp->wtext;
    WT_CONFIG_PARSER *config_parser = NULL;
    WT_CONFIG_ITEM k = {0}, v = {0};
    int ret = 0;

    if ((ret = wtext->config_parser_open_arg(wtext, NULL, config, &config_parser)) != 0) {
        LOG_ERROR(kp, NULL, "WT_EXTENSION_API.config_parser_open_arg: error: %d (%s)", ret,
          wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    /* Parse configuration key-value pairs */
    while ((ret = config_parser->next(config_parser, &k, &v)) == 0) {
        if (configure_int("verbose", &k, &v, &kp->verbose) == 0)
            continue;
        else if (configure_int("key_expires", &k, &v, &kp->key_expires) == 0)
            continue;

        LOG_ERROR(kp, NULL, "WT_CONFIG_PARSER.next: unexpected configuration: %.*s=%.*s",
          (int)k.len, k.str, (int)v.len, v.str);
        ret = EINVAL;
        goto err;
    }

    if (ret != WT_NOTFOUND) {
        LOG_ERROR(kp, NULL, "WT_CONFIG_PARSER.next: error: %d (%s)", ret,
          wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    ret = config_parser->close(config_parser);
    config_parser = NULL;
    if (ret != 0) {
        LOG_ERROR(kp, NULL, "WT_CONFIG_PARSER.close: error: %d (%s)", ret,
          wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    return (0);

err:
    if (config_parser != NULL)
        (void)config_parser->close(config_parser);

    return (ret);
}

/*
 * wiredtiger_extension_init --
 *     WiredTiger test key provider extension initialization.
 */
int
wiredtiger_extension_init(WT_CONNECTION *conn, WT_CONFIG_ARG *config)
{
    WT_EXTENSION_API *wtext = conn->get_extension_api(conn);

    /* Allocate the key provider structure */
    KEY_PROVIDER *kp;
    if ((kp = calloc(1, sizeof(KEY_PROVIDER))) == NULL) {
        wtext->err_printf(wtext, NULL, "%s: %s", __func__, wtext->strerror(wtext, NULL, ENOMEM));
        return (ENOMEM);
    }

    kp->wtext = wtext;
    kp->verbose = WT_VERBOSE_INFO; /* Default verbosity level */
    kp->key_expires = 43200;       /* Default: 12 hours = 43200 seconds */

    int ret = 0;
    WT_KEY_PROVIDER *wtkp = (WT_KEY_PROVIDER *)kp;

    /* Set initial key */
    if ((ret = kp_set_key(kp, &DEFAULT_KEY)) != 0) {
        LOG_ERROR(kp, NULL, "kp_set_key: %d (%s)", ret, wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    /* Parse configuration options */
    if ((ret = kp_configure(kp, config)) != 0)
        goto err;

    /* Initialize the key provider function table */
    wtkp->load_key = kp_load_key;
    wtkp->get_key = kp_get_key;
    wtkp->on_key_update = kp_on_key_update;
    wtkp->terminate = kp_terminate;

    /* Register the key provider with WiredTiger */
    if ((ret = conn->set_key_provider(conn, wtkp, NULL)) != 0) {
        LOG_ERROR(kp, NULL, "WT_CONNECTION.set_key_provider: %d (%s)", ret,
          wtext->strerror(wtext, NULL, ret));
        goto err;
    }

    LOG_INFO(kp, NULL,
      "Key provider initialized successfully; config: {verbose=%d, key_expires=%d}", kp->verbose,
      kp->key_expires);

    return (0);

err:
    if (kp != NULL)
        kp_terminate((WT_KEY_PROVIDER *)kp, NULL);

    return (ret);
}
