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
#include "test_util.h"

#define TIERED_STORAGE_CONFIG_FILE "wt-test-tiered-config.txt"

/*
 * testutil_tiered_begin --
 *     Begin processing for a test program that supports tiered storage.
 */
void
testutil_tiered_begin(TEST_OPTS *opts)
{
    WT_SESSION *session;

    testutil_assert(!opts->tiered_begun);
    testutil_assert(opts->conn != NULL);

    if (opts->tiered_storage && opts->tiered_flush_interval_us != 0) {
        /*
         * Initialize the time of the next flush_tier. We need a temporary session to do that.
         */
        testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
        testutil_tiered_flush_complete(opts, session, NULL);
        testutil_check(session->close(session, NULL));
    }

    opts->tiered_begun = true;
}

/*
 * testutil_tiered_sleep --
 *     Sleep for a number of seconds, or until it is time to flush_tier, or the process wants to
 *     exit.
 */
void
testutil_tiered_sleep(TEST_OPTS *opts, WT_SESSION *session, uint64_t seconds, bool *do_flush_tier)
{
    uint64_t now, wake_time;
    bool do_flush;

    now = testutil_time_us(session);
    wake_time = now + WT_MILLION * seconds;
    do_flush = false;
    if (do_flush_tier != NULL && opts->tiered_flush_next_us != 0 &&
      opts->tiered_flush_next_us < wake_time) {
        wake_time = opts->tiered_flush_next_us;
        do_flush = true;
    }
    *do_flush_tier = false;

    while (now < wake_time && opts->running) {
        /* Sleep a maximum of one second, so we can make sure we should still be running. */
        if (now + WT_MILLION < wake_time)
            __wt_sleep(1, 0);
        else
            __wt_sleep(0, wake_time - now);
        now = testutil_time_us(session);
    }
    if (opts->running && do_flush) {
        /* Don't flush again until we know this flush is complete. */
        opts->tiered_flush_next_us = 0;
        *do_flush_tier = true;
    }
}

/*
 * testutil_tiered_flush_complete --
 *     Notification that a flush_tier has completed, with the given argument.
 */
void
testutil_tiered_flush_complete(TEST_OPTS *opts, WT_SESSION *session, void *arg)
{
    uint64_t now;

    (void)arg;

    now = testutil_time_us(session);
    opts->tiered_flush_next_us = now + opts->tiered_flush_interval_us;
}

/*
 * tiered_storage_read_config --
 *     Read configuration from a file, if exists.
 */
static bool
tiered_storage_read_config(const char *home, char *s3_prefix, size_t s3_prefix_size)
{
    FILE *f;
    char config_path[512], str[512];
    char *s, *value;

    testutil_assert(s3_prefix_size > 0);
    s3_prefix[0] = '\0';

    testutil_check(__wt_snprintf(config_path, sizeof(config_path), "%s/%s",
      home == NULL ? "." : home, TIERED_STORAGE_CONFIG_FILE));
    f = fopen(config_path, "r");
    if (f == NULL) {
        testutil_assert_errno(errno == ENOENT);
        return (false);
    }

    /*
     * For now, we only support specifying prefixes in the file, but this can be easily expanded to
     * include more information, such as the bucket name.
     */
    while (fgets(str, sizeof(str), f) != NULL) {
        if (str[0] == '\0' || str[0] == '#')
            continue;
        s = str + strlen(str) - 1;
        if (*s == '\n')
            *s = '\0';
        if (str[0] == '\0')
            continue;

        value = strchr(str, '=');
        testutil_assertfmt(value != NULL, "Unexpected format of %s", config_path);
        *(value++) = '\0';

        if (strcmp(str, "prefix") == 0) {
            testutil_check(__wt_snprintf(s3_prefix, s3_prefix_size, "%s", value));
            continue;
        }

        testutil_die(EINVAL, "Unsupported key in the tiered storage config: %s", str);
    };

    /* Check that everything is specified. */
    testutil_assert(s3_prefix[0] != '\0');

    testutil_assert_errno(fclose(f) == 0);
    return (true);
}

/*
 * tiered_storage_write_config --
 *     Write configuration to a file.
 */
static void
tiered_storage_write_config(const char *home, const char *s3_prefix)
{
    FILE *f;
    char config_path[512];

    testutil_check(__wt_snprintf(config_path, sizeof(config_path), "%s/%s",
      home == NULL ? "." : home, TIERED_STORAGE_CONFIG_FILE));
    f = fopen(config_path, "w");
    testutil_assert_errno(f != NULL);

    testutil_assert_errno(fprintf(f, "# Tiered storage configuration written by testutil\n") >= 0);
    testutil_assert_errno(fprintf(f, "prefix=%s\n", s3_prefix) >= 0);

    testutil_assert_errno(fclose(f) == 0);
}

/*
 * tiered_storage_generate_prefix --
 *     Generate a unique prefix for objects when creating a new database; reuse the prefix when
 *     opening an existing database.
 */
static void
tiered_storage_generate_prefix(char *out, size_t size)
{
    struct tm time_parsed;
    size_t n;
#ifdef _WIN32
    __time64_t time_now;
#else
    time_t time_now;
#endif
    char time_str[100];

    /*
     * Generates a unique prefix to be used with the object keys, e.g.:
     * "s3test/test/2022-31-01-16-34-10/623843294--".
     *
     * Objects with the prefix pattern "s3test/" are deleted after a certain period of time
     * according to the lifecycle rule on the S3 bucket. Should you wish to make any changes to the
     * prefix pattern or lifecycle of the object, please speak to the release manager.
     */
#ifdef _WIN32
    time_now = _time64(NULL);
    testutil_check(_localtime64_s(&time_parsed, &time_now));
#else
    time_now = time(NULL);
    (void)localtime_r(&time_now, &time_parsed);
#endif
    n = strftime(time_str, sizeof(time_str), "%F-%H-%M-%S", &time_parsed);
    testutil_assert(n > 0);
    testutil_check(
      __wt_snprintf(out, size, "s3test/test/%s/%" PRIu32 "--", time_str, testutil_random(NULL)));
}

/*
 * testutil_tiered_storage_configuration --
 *     Set up tiered storage configuration.
 */
void
testutil_tiered_storage_configuration(TEST_OPTS *opts, const char *home, char *tiered_cfg,
  size_t tiered_cfg_size, char *ext_cfg, size_t ext_cfg_size)
{
    char auth_token[256];
    char cwd[256], dir[256], s3_prefix[128];
    const char *s3_access_key, *s3_secret_key, *s3_bucket_name;
    bool is_dir_store;

    s3_bucket_name = NULL;
    auth_token[0] = '\0';

    if (opts->tiered_storage) {
        is_dir_store = testutil_is_dir_store(opts);
        if (!is_dir_store) {
            s3_access_key = getenv("aws_sdk_s3_ext_access_key");
            s3_secret_key = getenv("aws_sdk_s3_ext_secret_key");
            s3_bucket_name = getenv("WT_S3_EXT_BUCKET");

            if (s3_access_key == NULL || s3_secret_key == NULL)
                testutil_die(EINVAL, "AWS S3 access key or secret key is not set");
            testutil_check(
              __wt_snprintf(auth_token, sizeof(auth_token), "%s;%s", s3_access_key, s3_secret_key));

            /*
             * By default the S3 bucket name is S3_DEFAULT_BUCKET_NAME, but it can be overridden
             * with environment variables.
             */
            if (s3_bucket_name == NULL)
                s3_bucket_name = S3_DEFAULT_BUCKET_NAME;

            /*
             * Read configuration that we might have saved before to a file, which is what we need
             * to do when opening an existing database (e.g., for tests that crash, recover, and
             * verify), so that we use the same object prefix.
             */
            if (!tiered_storage_read_config(home, s3_prefix, sizeof(s3_prefix))) {
                /* Generate a random prefix for the new database. */
                tiered_storage_generate_prefix(s3_prefix, sizeof(s3_prefix));

                /* Remember it for the next time. */
                tiered_storage_write_config(home, s3_prefix);
            }
        }
        testutil_check(__wt_snprintf(ext_cfg, ext_cfg_size, TESTUTIL_ENV_CONFIG_TIERED_EXT,
          opts->build_dir, opts->tiered_storage_source, opts->tiered_storage_source, opts->delay_ms,
          opts->error_ms, opts->force_delay, opts->force_error));

        if (is_dir_store) {
            if (opts->absolute_bucket_dir) {
                if (opts->home[0] == '/')
                    testutil_check(
                      __wt_snprintf(dir, sizeof(dir), "%s/%s", opts->home, DIR_STORE_BUCKET_NAME));
                else {
                    if (getcwd(cwd, sizeof(cwd)) == NULL)
                        testutil_die(ENOENT, "No such directory");
                    testutil_check(__wt_snprintf(
                      dir, sizeof(dir), "%s/%s/%s", cwd, opts->home, DIR_STORE_BUCKET_NAME));
                }
            } else
                testutil_check(__wt_snprintf(dir, sizeof(dir), "%s", DIR_STORE_BUCKET_NAME));
        }
        testutil_check(__wt_snprintf(tiered_cfg, tiered_cfg_size, TESTUTIL_ENV_CONFIG_TIERED,
          is_dir_store ? dir : s3_bucket_name, is_dir_store ? "pfx-" : s3_prefix,
          opts->local_retention, opts->tiered_storage_source, auth_token));
        if (is_dir_store && opts->make_bucket_dir) {
            testutil_check(
              __wt_snprintf(dir, sizeof(dir), "%s/%s", opts->home, DIR_STORE_BUCKET_NAME));
            testutil_check(mkdir(dir, 0777));
        }

    } else {
        testutil_check(__wt_snprintf(ext_cfg, ext_cfg_size, "\"\""));
        testutil_assert(tiered_cfg_size > 0);
        tiered_cfg[0] = '\0';
    }
}
