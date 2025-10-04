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
 *
 * ex_data_source.c
 * 	demonstrates how to create and access a data source
 */
#include <test_util.h>

/*! [WT_EXTENSION_API declaration] */
#include <wiredtiger_ext.h>

static WT_EXTENSION_API *wt_api;

static void
my_data_source_init(WT_CONNECTION *connection)
{
    wt_api = connection->get_extension_api(connection);
}
/*! [WT_EXTENSION_API declaration] */

/*! [WT_DATA_SOURCE alter] */
static int
my_alter(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE alter] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE create] */
static int
my_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE create] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)uri;
    (void)config;

    {
#if !defined(ERROR_BAD_COMMAND)
#define ERROR_BAD_COMMAND 37
#endif
        /*! [WT_EXTENSION_API map_windows_error] */
        int posix_error = wt_api->map_windows_error(wt_api, session, ERROR_BAD_COMMAND);
        /*! [WT_EXTENSION_API map_windows_error] */
        (void)posix_error;
    }

    {
        const char *msg = "string";
        /*! [WT_EXTENSION_API err_printf] */
        (void)wt_api->err_printf(wt_api, session, "extension error message: %s", msg);
        /*! [WT_EXTENSION_API err_printf] */
    }

    {
        const char *msg = "string";
        /*! [WT_EXTENSION_API msg_printf] */
        (void)wt_api->msg_printf(wt_api, session, "extension message: %s", msg);
        /*! [WT_EXTENSION_API msg_printf] */
    }

    {
        int ret = 0;
        /*! [WT_EXTENSION_API strerror] */
        (void)wt_api->err_printf(
          wt_api, session, "WiredTiger error return: %s", wt_api->strerror(wt_api, session, ret));
        /*! [WT_EXTENSION_API strerror] */
    }

    {
        /*! [WT_EXTENSION_API scr_alloc] */
        void *buffer;
        if ((buffer = wt_api->scr_alloc(wt_api, session, 512)) == NULL) {
            (void)wt_api->err_printf(
              wt_api, session, "buffer allocation: %s", session->strerror(session, ENOMEM));
            return (ENOMEM);
        }
        /*! [WT_EXTENSION_API scr_alloc] */

        /*! [WT_EXTENSION_API scr_free] */
        wt_api->scr_free(wt_api, session, buffer);
        /*! [WT_EXTENSION_API scr_free] */
    }

    return (0);
}

/*! [WT_DATA_SOURCE compact] */
static int
my_compact(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE compact] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE drop] */
static int
my_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE drop] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)config;

    return (0);
}

static int
data_source_cursor(void)
{
    return (0);
}

static const char *
data_source_error(int v)
{
    return (v == 0 ? "one" : "two");
}

static int
my_cursor_next(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}
static int
my_cursor_prev(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}
static int
my_cursor_reset(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}
static int
my_cursor_search(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}
static int
my_cursor_search_near(WT_CURSOR *wtcursor, int *exactp)
{
    (void)wtcursor;
    (void)exactp;
    return (0);
}
static int
my_cursor_insert(WT_CURSOR *wtcursor)
{
    WT_SESSION *session = NULL;

    /* Unused parameters */
    (void)wtcursor;

    {
        const char *key1 = NULL, *key2 = NULL;
        uint32_t key1_len = 0, key2_len = 0;
        WT_COLLATOR *collator = NULL;
        /*! [WT_EXTENSION collate] */
        WT_ITEM first, second;
        int cmp;

        first.data = key1;
        first.size = key1_len;
        second.data = key2;
        second.size = key2_len;

        error_check(wt_api->collate(wt_api, session, collator, &first, &second, &cmp));
        if (cmp == 0)
            printf("key1 collates identically to key2\n");
        else if (cmp < 0)
            printf("key1 collates less than key2\n");
        else
            printf("key1 collates greater than key2\n");
        /*! [WT_EXTENSION collate] */
    }

    return (0);
}

static int
my_cursor_update(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}
static int
my_cursor_remove(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}
static int
my_cursor_bound(WT_CURSOR *wtcursor, const char *config)
{
    (void)wtcursor;
    (void)config;
    return (0);
}
static int
my_cursor_close(WT_CURSOR *wtcursor)
{
    (void)wtcursor;
    return (0);
}

/*! [WT_DATA_SOURCE open_cursor] */
typedef struct __my_cursor {
    WT_CURSOR wtcursor; /* WiredTiger cursor, must come first */

    /*
     * Local cursor information: for example, we might want to have a reference to the extension
     * functions.
     */
    WT_EXTENSION_API *wtext; /* Extension functions */
} MY_CURSOR;

static int
my_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config,
  WT_CURSOR **new_cursor)
{
    MY_CURSOR *cursor;
    int ret;

    /* Allocate and initialize a WiredTiger cursor. */
    if ((cursor = calloc(1, sizeof(*cursor))) == NULL)
        return (errno);

    cursor->wtcursor.next = my_cursor_next;
    cursor->wtcursor.prev = my_cursor_prev;
    cursor->wtcursor.reset = my_cursor_reset;
    cursor->wtcursor.search = my_cursor_search;
    cursor->wtcursor.search_near = my_cursor_search_near;
    cursor->wtcursor.insert = my_cursor_insert;
    cursor->wtcursor.update = my_cursor_update;
    cursor->wtcursor.remove = my_cursor_remove;
    cursor->wtcursor.close = my_cursor_close;
    cursor->wtcursor.bound = my_cursor_bound;
    /*
     * Configure local cursor information.
     */

    /* Return combined cursor to WiredTiger. */
    *new_cursor = (WT_CURSOR *)cursor;

    /*! [WT_DATA_SOURCE open_cursor] */
    {
        (void)dsrc; /* Unused parameters */
        (void)session;
        (void)uri;
        (void)new_cursor;

        {
            /*! [WT_EXTENSION_CONFIG boolean] */
            WT_CONFIG_ITEM v;
            int my_data_source_overwrite;

            /*
             * Retrieve the value of the boolean type configuration string "overwrite".
             */
            error_check(wt_api->config_get(wt_api, session, config, "overwrite", &v));
            my_data_source_overwrite = v.val != 0;
            /*! [WT_EXTENSION_CONFIG boolean] */

            (void)my_data_source_overwrite;
        }

        {
            /*! [WT_EXTENSION_CONFIG integer] */
            WT_CONFIG_ITEM v;
            int64_t my_data_source_page_size;

            /*
             * Retrieve the value of the integer type configuration string "page_size".
             */
            error_check(wt_api->config_get(wt_api, session, config, "page_size", &v));
            my_data_source_page_size = v.val;
            /*! [WT_EXTENSION_CONFIG integer] */

            (void)my_data_source_page_size;
        }

        {
            /*! [WT_EXTENSION config_get] */
            WT_CONFIG_ITEM v;
            const char *my_data_source_key;

            /*
             * Retrieve the value of the string type configuration string "key_format".
             */
            error_check(wt_api->config_get(wt_api, session, config, "key_format", &v));

            /*
             * Values returned from WT_EXTENSION_API::config in the str field are not
             * nul-terminated; the associated length must be used instead.
             */
            if (v.len == 1 && v.str[0] == 'r')
                my_data_source_key = "recno";
            else
                my_data_source_key = "bytestring";
            /*! [WT_EXTENSION config_get] */

            (void)my_data_source_key;
        }

        {
            /*! [WT_EXTENSION collator config] */
            WT_COLLATOR *collator;
            int collator_owned;
            /*
             * Configure the appropriate collator.
             */
            error_check(wt_api->collator_config(
              wt_api, session, "dsrc:", config, &collator, &collator_owned));
            /*! [WT_EXTENSION collator config] */
        }

        /*! [WT_DATA_SOURCE error message] */
        /*
         * If an underlying function fails, log the error and then return a non-zero value.
         */
        if ((ret = data_source_cursor()) != 0) {
            (void)wt_api->err_printf(wt_api, session, "my_open_cursor: %s", data_source_error(ret));
            return (WT_ERROR);
        }
        /*! [WT_DATA_SOURCE error message] */

        {
            /*! [WT_EXTENSION metadata insert] */
            /*
             * Insert a new WiredTiger metadata record.
             */
            const char *key = "datasource_uri";
            const char *value = "data source uri's record";

            error_check(wt_api->metadata_insert(wt_api, session, key, value));
            /*! [WT_EXTENSION metadata insert] */
        }

        {
            /*! [WT_EXTENSION metadata remove] */
            /*
             * Remove a WiredTiger metadata record.
             */
            const char *key = "datasource_uri";

            error_check(wt_api->metadata_remove(wt_api, session, key));
            /*! [WT_EXTENSION metadata remove] */
        }

        {
            /*! [WT_EXTENSION metadata search] */
            /*
             * Search for a WiredTiger metadata record.
             */
            const char *key = "datasource_uri";
            char *value;

            error_check(wt_api->metadata_search(wt_api, session, key, &value));
            printf("metadata: %s has a value of %s\n", key, value);
            /*! [WT_EXTENSION metadata search] */
        }

        {
            /*! [WT_EXTENSION metadata update] */
            /*
             * Update a WiredTiger metadata record (insert it if it does not yet exist, update it if
             * it does).
             */
            const char *key = "datasource_uri";
            const char *value = "data source uri's record";

            error_check(wt_api->metadata_update(wt_api, session, key, value));
            /*! [WT_EXTENSION metadata update] */
        }
    }
    return (0);
}

/*! [WT_DATA_SOURCE rename] */
static int
my_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, const char *newname,
  WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE rename] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)newname;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE salvage] */
static int
my_salvage(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE salvage] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE size] */
static int
my_size(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, wt_off_t *size)
/*! [WT_DATA_SOURCE size] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)size;

    return (0);
}

/*! [WT_DATA_SOURCE truncate] */
static int
my_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE truncate] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE range truncate] */
static int
my_range_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session, WT_CURSOR *start, WT_CURSOR *stop)
/*! [WT_DATA_SOURCE range truncate] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)start;
    (void)stop;

    return (0);
}

/*! [WT_DATA_SOURCE verify] */
static int
my_verify(WT_DATA_SOURCE *dsrc, WT_SESSION *session, const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE verify] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)uri;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE checkpoint] */
static int
my_checkpoint(WT_DATA_SOURCE *dsrc, WT_SESSION *session, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE checkpoint] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;
    (void)config;

    return (0);
}

/*! [WT_DATA_SOURCE terminate] */
static int
my_terminate(WT_DATA_SOURCE *dsrc, WT_SESSION *session)
/*! [WT_DATA_SOURCE terminate] */
{
    /* Unused parameters */
    (void)dsrc;
    (void)session;

    return (0);
}

static const char *home;

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;

    home = example_setup(argc, argv);

    error_check(wiredtiger_open(home, NULL, "create", &conn));
    my_data_source_init(conn);

    {
        /*! [WT_DATA_SOURCE register] */
        static WT_DATA_SOURCE my_dsrc = {my_alter, my_create, my_compact, my_drop, my_open_cursor,
          my_rename, my_salvage, my_size, my_truncate, my_range_truncate, my_verify, my_checkpoint,
          my_terminate};
        error_check(conn->add_data_source(conn, "dsrc:", &my_dsrc, NULL));
        /*! [WT_DATA_SOURCE register] */
    }

    /*! [WT_DATA_SOURCE configure boolean] */
    /* my_boolean defaults to true. */
    error_check(conn->configure_method(
      conn, "WT_SESSION.open_cursor", NULL, "my_boolean=true", "boolean", NULL));
    /*! [WT_DATA_SOURCE configure boolean] */

    /*! [WT_DATA_SOURCE configure integer] */
    /* my_integer defaults to 5. */
    error_check(
      conn->configure_method(conn, "WT_SESSION.open_cursor", NULL, "my_integer=5", "int", NULL));
    /*! [WT_DATA_SOURCE configure integer] */

    /*! [WT_DATA_SOURCE configure string] */
    /* my_string defaults to "name". */
    error_check(conn->configure_method(
      conn, "WT_SESSION.open_cursor", NULL, "my_string=name", "string", NULL));
    /*! [WT_DATA_SOURCE configure string] */

    /*! [WT_DATA_SOURCE configure list] */
    /* my_list defaults to "first" and "second". */
    error_check(conn->configure_method(
      conn, "WT_SESSION.open_cursor", NULL, "my_list=[first, second]", "list", NULL));
    /*! [WT_DATA_SOURCE configure list] */

    /*! [WT_DATA_SOURCE configure integer with checking] */
    /*
     * Limit the number of devices to between 1 and 30; the default is 5.
     */
    error_check(conn->configure_method(
      conn, "WT_SESSION.open_cursor", NULL, "devices=5", "int", "min=1, max=30"));
    /*! [WT_DATA_SOURCE configure integer with checking] */

    /*! [WT_DATA_SOURCE configure string with checking] */
    /*
     * Limit the target string to one of /device, /home or /target; default to /home.
     */
    error_check(conn->configure_method(conn, "WT_SESSION.open_cursor", NULL, "target=/home",
      "string", "choices=[/device, /home, /target]"));
    /*! [WT_DATA_SOURCE configure string with checking] */

    /*! [WT_DATA_SOURCE configure list with checking] */
    /*
     * Limit the paths list to one or more of /device, /home, /mnt or
     * /target; default to /mnt.
     */
    error_check(conn->configure_method(conn, "WT_SESSION.open_cursor", NULL, "paths=[/mnt]", "list",
      "choices=[/device, /home, /mnt, /target]"));
    /*! [WT_DATA_SOURCE configure list with checking] */

    /*! [WT_EXTENSION_API default_session] */
    (void)wt_api->msg_printf(wt_api, NULL, "configuration complete");
    /*! [WT_EXTENSION_API default_session] */

    error_check(conn->close(conn, NULL));

    return (EXIT_SUCCESS);
}
