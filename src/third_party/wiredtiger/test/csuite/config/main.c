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
 * This program tests precompiling configurations for APIs that support it. The test is exhaustive
 * in a way, that we try setting every key to various kinds of valid and invalid values. We turn on
 * a verbose setting that gives us information in the callback that, for the valid configurations,
 * allows us do further checking. We confirm that the resulting compiled configuration, rendering in
 * string form, matches the what we expect from the inputs to the compilation.
 *
 */

/*
 * This structure holds the state of parsing given by verbose callbacks. We turn on verbose when we
 * compile configurations. We get callbacks for these verbose messages, and in this structure we
 * collect the set of inputs going into each compilation. When we get a verbose callback with the
 * reconstituted result of the compilation (a string representation of the compiled object), we can
 * check to make sure it matches the inputs.
 */
#define MAX_INPUT_CONFIG 5
typedef struct {
    int completed;
    char *method_name;
    char *input_config[MAX_INPUT_CONFIG];
    u_int input_config_count;
} PARSE_STATE;

/*
 * Create our own event handler structure, it will be given back to use in event handler callbacks.
 */
typedef struct {
    WT_EVENT_HANDLER h;
    PARSE_STATE state;
    bool expect_errors;
} CUSTOM_EVENT_HANDLER;

/*
 * A list of valid and invalid value combinations for a given key.
 */
typedef struct key_values {
    const char *key;
    const char **valid;
    const char **invalid;
    struct key_values *subcategory;
} KEY_VALUES;

/*
 * COPY_MESSAGE_CONTENT --
 *   Given a source string that looks like:
 *     some prefix: "foo bar"
 *   make a copy of the part in double quotes and put it in dest, without modifying the source.
 */
#define COPY_MESSAGE_CONTENT(dest, src)        \
    do {                                       \
        char *_dest, *_src, *_trailing;        \
                                               \
        _src = strchr(src, ':') + 1;           \
        while (*_src == ' ')                   \
            ++_src;                            \
        if (*_src == '"')                      \
            ++_src;                            \
        _dest = dstrdup(_src);                 \
        _trailing = &_dest[strlen(_dest) - 1]; \
        if (*_trailing == '"')                 \
            *_trailing = '\0';                 \
        dest = _dest;                          \
    } while (0)

/*
 * For various standard types, a list of valid and invalid strings to try.
 */
static const char *valid_bool[] = {"true", "false", "0", "1", "", NULL};
static const char *invalid_bool[] = {"foo", "TRUE", "FALSE", "-2", NULL};

static const char *valid_string[] = {"foo", "", "42", "-42", NULL};
static const char *invalid_string[] = {"+$", NULL};

static const char *valid_nonnegative_int[] = {"42", "0", NULL};
static const char *invalid_nonnegative_int[] = {"foo", "", "-42", NULL};

/*
 * Check compiling configuration strings for begin_transaction.
 *
 *   "ignore_prepare=%s,isolation=%s,name=%s,no_timestamp=%s,"
 *   "operation_timeout_ms=%s,priority=%s,read_timestamp=%s,"
 *   "roundup_timestamps=(prepared=%s,read=%s),sync=%s";
 */

/*
 * These are special valid/invalid pairs used with specific keys in configuration for
 * begin_transaction. Every choice list implicitly allows a blank value.
 */
static const char *valid_ignore_prepare[] = {"true", "false", "force", "", NULL};
static const char *invalid_ignore_prepare[] = {"foo", NULL};

static const char *valid_isolation[] = {"read-uncommitted", "read-committed", "snapshot", "", NULL};
static const char *invalid_isolation[] = {"foo", NULL};

/* -100 to 100 allowed */
static const char *valid_priority_int[] = {"0", "-100", "100", NULL};
static const char *invalid_priority_int[] = {"foo", "-101", "101", NULL};

/*
 * The complete set of configuration keys for begin_transaction, along with valid/invalid pairs for
 * each.
 */
static KEY_VALUES begin_txn_roundup_kv[] = {{"prepared", valid_bool, invalid_bool, NULL},
  {"read", valid_bool, invalid_bool, NULL}, {NULL, NULL, NULL, NULL}};
static KEY_VALUES begin_txn_kv[] = {
  {"ignore_prepare", valid_ignore_prepare, invalid_ignore_prepare, NULL},
  {"isolation", valid_isolation, invalid_isolation, NULL},
  {"name", valid_string, invalid_string, NULL}, {"no_timestamp", valid_bool, invalid_bool, NULL},
  {"operation_timeout_ms", valid_nonnegative_int, invalid_nonnegative_int, NULL},
  {"priority", valid_priority_int, invalid_priority_int, NULL},
  /*
   * As far as the parser is concerned, the read timestamp is just a string. It is validated by each
   * API configuration code. This is likely to change, we'll probably introduce a timestamp type,
   * until then, this is the best we can do.
   */
  {"read_timestamp", valid_string, invalid_string, NULL},
  {"roundup_timestamps", NULL, NULL, begin_txn_roundup_kv},
  {"sync", valid_bool, invalid_bool, NULL}, {NULL, NULL, NULL, NULL}};

/*
 * free_parse_state --
 *     Free and zero the state to prepare for another set of callbacks.
 */
static void
free_parse_state(PARSE_STATE *state)
{
    int i;

    free(state->method_name);
    state->method_name = NULL;
    for (i = 0; i < MAX_INPUT_CONFIG; ++i) {
        free(state->input_config[i]);
        state->input_config[i] = NULL;
    }
    state->input_config_count = 0;
}

/*
 * check_single_result_against_inputs --
 *     Check to see that a k/v pair obtained from a compilation result matches one of the inputs.
 */
static void
check_single_result_against_inputs(
  PARSE_STATE *state, const char *prefix, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
{
    WT_CONFIG_ITEM expect_value;
    WT_CONFIG_PARSER *parser;
    int pos;
    int ret;
    char keystr[1024];
    const char *s;

    testutil_assert(key->len + strlen(prefix) + 1 < sizeof(keystr));
    testutil_snprintf(keystr, sizeof(keystr), "%s%.*s", prefix, (int)key->len, key->str);

    /*
     * Check the inputs in reverse order. The zero-th would be the default, and we want to check
     * that last. This is slow, but we don't care too much.
     */
    for (pos = (int)state->input_config_count - 1; pos >= 0; --pos) {
        s = state->input_config[pos];
        testutil_check(wiredtiger_config_parser_open(NULL, s, strlen(s), &parser));
        ret = parser->get(parser, keystr, &expect_value);
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);
        testutil_check(parser->close(parser));

        if (ret == 0) {
            testutil_assert(value->val == expect_value.val);
            if (value->type == WT_CONFIG_ITEM_BOOL) {
                /*
                 * WiredTiger code that uses the config parser merely looks at value->val being 0 or
                 * 1. Because of this, numerical 0 and 1 are allowed in input for cases that expect
                 * boolean. Since the original config parsing allows for this, the config compiler
                 * does too, but it silently converts the 0 and 1 numeric item to boolean items so
                 * they can be accessed quickly and uniformly. When we reconstruct the
                 * configuration string from the compiled string, it is a standard "true"/"false",
                 * even when the input is 1/0.
                 */
                if (value->val == 0)
                    testutil_assert(strncmp("false", value->str, value->len) == 0);
                else
                    testutil_assert(strncmp("true", value->str, value->len) == 0);
            } else if (value->type == WT_CONFIG_ITEM_STRUCT) {
                testutil_assert(value->type == expect_value.type);
                /*TODO*/
            } else {
                testutil_assert(value->type == expect_value.type);
                testutil_assert(value->len == expect_value.len);
                testutil_assert(strncmp(value->str, expect_value.str, value->len) == 0);
            }
            return;
        }
    }

    /*
     * This should never happen, we should always find the key, at least in the defaults. The
     * compiled result seems to show a key that isn't in the allowed list of keys.
     */
    testutil_assert(false);
}

/*
 * check_configuration_result --
 *     Check a configuration compilation result against saved state about the inputs.
 */
static void
check_configuration_result(
  PARSE_STATE *state, const char *prefix, const char *result, size_t result_len)
{
    WT_CONFIG_ITEM key, value;
    WT_CONFIG_PARSER *parser;
    int ret;
    char new_prefix[1024];

    testutil_check(wiredtiger_config_parser_open(NULL, result, result_len, &parser));
    while ((ret = parser->next(parser, &key, &value)) == 0) {
        if (value.type == WT_CONFIG_ITEM_STRUCT) {
            testutil_assert(value.str[0] == '(' && value.str[value.len - 1] == ')');
            testutil_snprintf(
              new_prefix, sizeof(new_prefix), "%s%.*s.", prefix, (int)key.len, key.str);
            check_configuration_result(state, new_prefix, &value.str[1], value.len - 2);
        } else
            check_single_result_against_inputs(state, prefix, &key, &value);
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(parser->close(parser));
}

/*
 * handle_wiredtiger_error --
 *     Handle error callbacks from WiredTiger.
 */
static int
handle_wiredtiger_error(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    CUSTOM_EVENT_HANDLER *custom;

    (void)session;

    /* Cast the handler back to our custom handler. */
    custom = (CUSTOM_EVENT_HANDLER *)handler;

    if (custom->expect_errors)
        return (0);

    printf("ERROR: %s: %s\n", wiredtiger_strerror(error), message);
    return (1);
}

/*
 * handle_wiredtiger_message --
 *     Handle message callbacks from WiredTiger.
 */
static int
handle_wiredtiger_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    CUSTOM_EVENT_HANDLER *custom;
    char *output, *p;

    (void)session;

    /* Cast the handler back to our custom handler. */
    custom = (CUSTOM_EVENT_HANDLER *)handler;

    /* printf("MESSAGE: %s\n", message); */
    if ((p = strstr(message, "for method:")) != NULL) {
        free_parse_state(&custom->state);
        COPY_MESSAGE_CONTENT(custom->state.method_name, p);
    } else if ((p = strstr(message, "input config:")) != NULL) {
        testutil_assert(custom->state.input_config_count < MAX_INPUT_CONFIG);
        COPY_MESSAGE_CONTENT(custom->state.input_config[custom->state.input_config_count++], p);
    } else if ((p = strstr(message, "reconstructed config:")) != NULL) {
        /*
         * At this point, the compilation was successful. If that is unexpected, complain now.
         */
        if (custom->expect_errors) {
            fprintf(stderr, "Compilation succeeded, but expected it to fail\n");
            return (1);
        }
        COPY_MESSAGE_CONTENT(output, p);
        /* printf("CHECKING: %s\n", output); */
        check_configuration_result(&custom->state, "", output, strlen(output));
        ++custom->state.completed;
        free(output);
        free_parse_state(&custom->state);
    } else {
        /*
         * We know how to handle every verbose message from configuration. Yell if that changes.
         */
        fprintf(stderr, "UNKNOWN verbose message: %s\n", message);
        return (1);
    }
    return (0);
}

/*
 * check_compiling_one_configuration --
 *     Compile a configuration setting a key to a given value.
 */
static void
check_compiling_one_configuration(WT_CONNECTION *conn, const char *method_name,
  const char *subcat_name, const char *key, const char *val, bool expect_success)
{
    int ret;
    char config[1024];
    const char *compiled_ptr;
    bool got_success;

    if (subcat_name != NULL)
        testutil_snprintf(config, sizeof(config), "%s=(%s=%s)", subcat_name, key, val);
    else
        testutil_snprintf(config, sizeof(config), "%s=%s", key, val);
    ret = conn->compile_configuration(conn, method_name, config, &compiled_ptr);
    got_success = (ret == 0);
    if (got_success != expect_success)
        fprintf(stderr, "%s: config=\"%s\", expected %s, got return %s\n", method_name, config,
          (expect_success ? "success" : "failure"), wiredtiger_strerror(ret));
    testutil_assert(got_success == expect_success);
}

/*
 * check_compiling_invalid_configurations --
 *     Walk through the key/value list, compiling each combination. Each is expected to fail.
 */
static void
check_compiling_invalid_configurations(
  WT_CONNECTION *conn, const char *method_name, const char *subcat_name, KEY_VALUES *kv_list)
{
    KEY_VALUES *kv;
    const char **values;

    for (kv = kv_list; kv->key != NULL; ++kv)
        if (kv->subcategory != NULL) {
            /*
             * It's legal to have levels of subcategories, but this test program does not yet handle
             * it.
             */
            testutil_assert(subcat_name == NULL);
            check_compiling_invalid_configurations(conn, method_name, kv->key, kv->subcategory);
        } else
            for (values = kv->invalid; *values != NULL; ++values)
                check_compiling_one_configuration(
                  conn, method_name, subcat_name, kv->key, *values, false);
}

/*
 * check_compiling_valid_configurations --
 *     Walk through the key/value list, compiling each combination. Each is expected to succeed.
 */
static void
check_compiling_valid_configurations(
  WT_CONNECTION *conn, const char *method_name, const char *subcat_name, KEY_VALUES *kv_list)
{
    KEY_VALUES *kv;
    const char **values;

    for (kv = kv_list; kv->key != NULL; ++kv)
        if (kv->subcategory != NULL) {
            /*
             * It's legal to have levels of subcategories, but this test program does not yet handle
             * it.
             */
            testutil_assert(subcat_name == NULL);
            check_compiling_valid_configurations(conn, method_name, kv->key, kv->subcategory);
        } else
            for (values = kv->valid; *values != NULL; ++values)
                check_compiling_one_configuration(
                  conn, method_name, subcat_name, kv->key, *values, true);
}

/*
 * check_compiling_configurations --
 *     Here we do a pretty extensive check of all begin_transaction configurations. The verbose
 *     message checker helps to verify that the results from compiling are valid and match in the
 *     input.
 */
static void
check_compiling_configurations(TEST_OPTS *opts, CUSTOM_EVENT_HANDLER *handler)
{
    int ret;
    const char **bad_config;
    const char *compiled_ptr, *compiled_ptr2;
    static const char *bad_configs[] = {
      "789=value", "=value", "unknown_key=value", "++", "%s", "%", NULL};

    handler->expect_errors = true;

    /*
     * Do some basic checks to make sure the parser rejects bad configurations that don't have
     * anything to do with valid keys.
     */
    for (bad_config = bad_configs; *bad_config != NULL; ++bad_config) {
        ret = opts->conn->compile_configuration(
          opts->conn, "WT_SESSION.begin_transaction", *bad_config, &compiled_ptr);
        testutil_assert(ret != 0);
    }

    /* We shouldn't be able to compile for a method that doesn't support it. */
    ret = opts->conn->compile_configuration(opts->conn, "WT_SESSION.create", "", &compiled_ptr);
    testutil_assert(ret != 0);

    /* Compile with a parameter to be bound. */
    handler->expect_errors = false;
    testutil_check(opts->conn->compile_configuration(
      opts->conn, "WT_SESSION.begin_transaction", "isolation=%s", &compiled_ptr));

    /* We cannot use the resulting compiled string without binding it. */
    handler->expect_errors = true;
    ret = opts->session->begin_transaction(opts->session, compiled_ptr);
    testutil_assert(ret != 0);

    /* Bind the parameter now. */
    handler->expect_errors = false;
    testutil_check(opts->session->bind_configuration(opts->session, compiled_ptr, "snapshot"));

    /* Now we can use the API. */
    testutil_check(opts->session->begin_transaction(opts->session, compiled_ptr));
    testutil_check(opts->session->rollback_transaction(opts->session, NULL));

    /* We should be able to compile an empty string. */
    handler->expect_errors = false;
    ret = opts->conn->compile_configuration(
      opts->conn, "WT_SESSION.begin_transaction", "", &compiled_ptr);
    testutil_assert(ret == 0);

    /* We shouldn't be able to use the result of a successful compile. */
    handler->expect_errors = true;
    ret = opts->conn->compile_configuration(
      opts->conn, "WT_SESSION.begin_transaction", compiled_ptr, &compiled_ptr2);
    testutil_assert(ret != 0);

    /* Check that the parser rejects invalid values for every key. */
    check_compiling_invalid_configurations(
      opts->conn, "WT_SESSION.begin_transaction", NULL, begin_txn_kv);

    /* Check that the parser accepts valid values for every key. */
    handler->expect_errors = false;
    check_compiling_valid_configurations(
      opts->conn, "WT_SESSION.begin_transaction", NULL, begin_txn_kv);
}

/*
 * main --
 *     The main entry point for a simple test/benchmark for compiling configuration strings.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    CUSTOM_EVENT_HANDLER event_handler;

    memset(&event_handler, 0, sizeof(event_handler));
    event_handler.h.handle_error = handle_wiredtiger_error;
    event_handler.h.handle_message = handle_wiredtiger_message;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_recreate_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, &event_handler.h,
      "create,statistics=(all),statistics_log=(json,on_close,wait=1),verbose=(configuration:2)",
      &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &opts->session));

    check_compiling_configurations(opts, &event_handler);

    /*
     * Make sure we've been able to successfully check the expected number of configuration
     * compilations. When this tests other APIs, or other APIs are made to be compilable, this
     * number will need to change. What we get by having an exact number is protection against the
     * WiredTiger verbose output changing or being reduced in the future. If that were to happen, we
     * want to fail, since without verbose feedback, we aren't doing the checks that we need.
     */
    printf(
      "checked %d successful configuration compilation outputs\n", event_handler.state.completed);
    testutil_assert(event_handler.state.completed == 45);

    free_parse_state(&event_handler.state);
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
