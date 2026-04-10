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

/*
 * testutil_disagg_storage_configuration --
 *     Set up disagg storage configuration.
 */
void
testutil_disagg_storage_configuration(TEST_OPTS *opts, const char *home, char *disagg_cfg,
  size_t disagg_cfg_size, char *ext_cfg, size_t ext_cfg_size)
{
    (void)home;
    char key_provider_ext_cfg[256];

    if (opts->disagg.is_enabled) {
        testutil_snprintf(ext_cfg, ext_cfg_size, TESTUTIL_ENV_CONFIG_DISAGG_EXT, opts->build_dir,
          opts->disagg.page_log, opts->disagg.page_log, opts->disagg.page_log_home, opts->delay_ms,
          opts->error_ms, opts->force_delay, opts->force_error, opts->disagg.page_log_map_size_mb,
          opts->disagg.page_log_verbose);

        if (opts->disagg.key_provider) {
            testutil_snprintf(key_provider_ext_cfg, sizeof(key_provider_ext_cfg),
              TESTUTIL_ENV_CONFIG_KEY_PROVIDER_EXT, opts->build_dir);
            testutil_strcat(
              ext_cfg, ext_cfg_size + sizeof(key_provider_ext_cfg), key_provider_ext_cfg);
        }

        testutil_snprintf(disagg_cfg, disagg_cfg_size, TESTUTIL_ENV_CONFIG_DISAGG,
          opts->disagg.mode, opts->disagg.page_log, opts->disagg.drain_threads,
          (opts->disagg.internal_page_delta ? "true" : "false"),
          (opts->disagg.leaf_page_delta ? "true" : "false"));
    } else {
        testutil_snprintf(ext_cfg, ext_cfg_size, "\"\"");
        testutil_assert(disagg_cfg_size > 0);
        disagg_cfg[0] = '\0';
    }
}

/*
 * preserve_copy_uri --
 *     Copy a uri from one connection to another.
 */
static void
preserve_copy_uri(
  WT_SESSION *from_session, const char *from_uri, WT_SESSION *to_session, const char *to_uri)
{
    WT_CURSOR *from, *to;
    WT_DECL_RET;
    WT_ITEM key, value;
    char new_config[256];

    ret = from_session->open_cursor(from_session, from_uri, NULL, "raw", &from);
    if (ret == WT_NOTFOUND || ret == ENOENT) {
        fprintf(
          stderr, "%s: file not found during preserve: %s\n", from_uri, wiredtiger_strerror(ret));
        return;
    }
    testutil_check(ret);
    testutil_snprintf(new_config, sizeof(new_config), "key_format=%s,value_format=%s",
      from->key_format, from->value_format);
    testutil_check(to_session->create(to_session, to_uri, new_config));
    testutil_check(to_session->open_cursor(to_session, to_uri, NULL, "raw", &to));

    WT_CLEAR(key);
    WT_CLEAR(value);
    while ((ret = from->next(from)) == 0) {
        from->get_key(from, &key);
        from->get_value(from, &value);
        to->set_key(to, &key);
        to->set_value(to, &value);
        testutil_check(to->insert(to));
    }
    testutil_assert(ret == 0 || ret == WT_NOTFOUND);
    testutil_check(from->close(from));
    testutil_check(to->close(to));
}

#define LAYERED_PREFIX "layered:"

/*
 * testutil_disagg_preserve --
 *     Save the components of disaggregated and layered tables to regular local tables. The ingest
 *     table, the stable table and the composite view of the layered table is saved, for layered
 *     tables found in the metadata. This is typically called after a failure has occurred.
 */
void
testutil_disagg_preserve(WT_CONNECTION *conn, const char *subdir)
{
    WT_CONNECTION *dest_conn;
    WT_CURSOR *metacur;
    WT_DECL_RET;
    WT_ITEM dest_dir, from_uri, to_uri;
    WT_SESSION *session, *dest_session;
    const char *base, *home, *uri;

    home = conn->get_home(conn);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    WT_CLEAR(dest_dir);
    WT_CLEAR(from_uri);
    WT_CLEAR(to_uri);

    /* Create a new WiredTiger instance to hold the preserved files. */
    testutil_format_item(&dest_dir, "%s/%s", home, subdir);
    testutil_recreate_dir((char *)dest_dir.data);
    fprintf(stderr, "preserving ingest/stable/layered to %s\n", (char *)dest_dir.data);
    testutil_check(wiredtiger_open((char *)dest_dir.data, NULL, "create", &dest_conn));
    testutil_check(dest_conn->open_session(dest_conn, NULL, NULL, &dest_session));

    /* Copy the metadata file first. */
    preserve_copy_uri(session, "metadata:", dest_session, "table:metadata.preserve");

    /*
     * Now, for each layered table in the metadata, copy its layered component to a newly created
     * preserve table.
     */
    testutil_check(session->open_cursor(session, "metadata:", NULL, NULL, &metacur));
    while ((ret = metacur->next(metacur)) == 0) {
        metacur->get_key(metacur, &uri);
        if (WT_PREFIX_MATCH(uri, LAYERED_PREFIX)) {
            /*
             * Preserved files cannot be named with the strings ".wt_ingest" or ".wt_stable"
             * embedded in the names, as WiredTiger will treat these specially.
             */
            base = uri + strlen(LAYERED_PREFIX);
            testutil_format_item(&from_uri, "file:%s.wt_ingest", base);
            testutil_format_item(&to_uri, "table:%s.ingest_preserve", base);
            preserve_copy_uri(session, (char *)from_uri.data, dest_session, (char *)to_uri.data);

            testutil_format_item(&from_uri, "file:%s.wt_stable", base);
            testutil_format_item(&to_uri, "table:%s.stable_preserve", base);
            preserve_copy_uri(session, (char *)from_uri.data, dest_session, (char *)to_uri.data);

            testutil_format_item(&to_uri, "table:%s.layered_preserve", base);
            preserve_copy_uri(session, uri, dest_session, (char *)to_uri.data);
        }
    }
    testutil_assert(ret == WT_NOTFOUND);

    testutil_check(metacur->close(metacur));
    free(dest_dir.mem);
    free(from_uri.mem);
    free(to_uri.mem);
    testutil_check(session->close(session, NULL));

    /* Final checkpoint for destination and close everything. */
    testutil_check(dest_session->checkpoint(dest_session, NULL));
    testutil_check(dest_session->close(dest_session, NULL));
    testutil_check(dest_conn->close(dest_conn, NULL));
}
