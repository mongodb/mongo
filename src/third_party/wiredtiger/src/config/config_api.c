/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __config_parser_close --
 *     WT_CONFIG_PARSER->close method.
 */
static int
__config_parser_close(WT_CONFIG_PARSER *wt_config_parser)
{
    WT_CONFIG_PARSER_IMPL *config_parser;

    config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

    __wt_free(config_parser->session, config_parser);
    return (0);
}

/*
 * __config_parser_get --
 *     WT_CONFIG_PARSER->search method.
 */
static int
__config_parser_get(WT_CONFIG_PARSER *wt_config_parser, const char *key, WT_CONFIG_ITEM *cval)
{
    WT_CONFIG_PARSER_IMPL *config_parser;

    config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

    return (__wt_config_subgets(config_parser->session, &config_parser->config_item, key, cval));
}

/*
 * __config_parser_next --
 *     WT_CONFIG_PARSER->next method.
 */
static int
__config_parser_next(WT_CONFIG_PARSER *wt_config_parser, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *cval)
{
    WT_CONFIG_PARSER_IMPL *config_parser;

    config_parser = (WT_CONFIG_PARSER_IMPL *)wt_config_parser;

    return (__wt_config_next(&config_parser->config, key, cval));
}

/*
 * wiredtiger_config_parser_open --
 *     Create a configuration parser.
 */
int
wiredtiger_config_parser_open(
  WT_SESSION *wt_session, const char *config, size_t len, WT_CONFIG_PARSER **config_parserp)
{
    static const WT_CONFIG_PARSER stds = {
      __config_parser_close, __config_parser_next, __config_parser_get};
    WT_CONFIG_ITEM config_item = {config, len, 0, WT_CONFIG_ITEM_STRING};
    WT_CONFIG_PARSER_IMPL *config_parser;
    WT_SESSION_IMPL *session;

    *config_parserp = NULL;

    session = (WT_SESSION_IMPL *)wt_session;

    WT_RET(__wt_calloc_one(session, &config_parser));
    config_parser->iface = stds;
    config_parser->session = session;

    /*
     * Setup a WT_CONFIG_ITEM to be used for get calls and a WT_CONFIG structure for iterations
     * through the configuration string.
     */
    memcpy(&config_parser->config_item, &config_item, sizeof(config_item));
    __wt_config_initn(session, &config_parser->config, config, len);

    *config_parserp = (WT_CONFIG_PARSER *)config_parser;
    return (0);
}

/*
 * __config_validate --
 *     Validate a configuration string. Taking a function pointer to the matching function for the
 *     given configuration set.
 */
static int
__config_validate(WT_SESSION *wt_session, WT_EVENT_HANDLER *event_handler, const char *name,
  const char *config, const WT_CONFIG_ENTRY *config_matcher(const char *))
{
    const WT_CONFIG_ENTRY *ep, **epp;
    WT_CONNECTION_IMPL *conn, dummy_conn;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)wt_session;

    /*
     * It's a logic error to specify both a session and an event handler.
     */
    if (session != NULL && event_handler != NULL)
        WT_RET_MSG(session, EINVAL,
          "wiredtiger_config_validate event handler ignored when a session also specified");

    /*
     * If we're not given a session, but we do have an event handler, build a fake
     * session/connection pair and configure the event handler.
     */
    conn = NULL;
    if (session == NULL && event_handler != NULL) {
        WT_CLEAR(dummy_conn);
        conn = &dummy_conn;
        session = conn->default_session = &conn->dummy_session;
        session->iface.connection = &conn->iface;
        session->name = "wiredtiger_config_validate";
        __wt_event_handler_set(session, event_handler);
    }
    if (session != NULL)
        conn = S2C(session);

    if (name == NULL)
        WT_RET_MSG(session, EINVAL, "no name specified");
    if (config == NULL)
        WT_RET_MSG(session, EINVAL, "no configuration specified");

    /*
     * If we don't have a real connection, look for a matching name in the static list, otherwise
     * look in the configuration list (which has any configuration information the application has
     * added).
     */
    if (session == NULL || conn == NULL || conn->config_entries == NULL)
        ep = config_matcher(name);
    else {
        ep = NULL;
        for (epp = conn->config_entries; *epp != NULL && (*epp)->method != NULL; ++epp)
            if (strcmp((*epp)->method, name) == 0) {
                ep = *epp;
                break;
            }
    }
    if (ep == NULL)
        WT_RET_MSG(session, EINVAL, "unknown or unsupported configuration API: %s", name);

    return (__wt_config_check(session, ep, config, 0));
}

/*
 * wiredtiger_config_validate --
 *     Validate a configuration string.
 */
int
wiredtiger_config_validate(
  WT_SESSION *wt_session, WT_EVENT_HANDLER *event_handler, const char *name, const char *config)
{
    return (__config_validate(wt_session, event_handler, name, config, __wt_conn_config_match));
}

/*
 * wiredtiger_test_config_validate --
 *     Validate a test configuration string.
 */
int
wiredtiger_test_config_validate(
  WT_SESSION *wt_session, WT_EVENT_HANDLER *event_handler, const char *name, const char *config)
{
    return (__config_validate(wt_session, event_handler, name, config, __wt_test_config_match));
}

/*
 * __conn_foc_add --
 *     Add a new entry into the connection's free-on-close list.
 */
static void
__conn_foc_add(WT_SESSION_IMPL *session, const void *p)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /*
     * Callers of this function are expected to be holding the connection's api_lock.
     *
     * All callers of this function currently ignore errors.
     */
    if (__wt_realloc_def(session, &conn->foc_size, conn->foc_cnt + 1, &conn->foc) == 0)
        conn->foc[conn->foc_cnt++] = (void *)p;
}

/*
 * __wt_conn_foc_discard --
 *     Discard any memory the connection accumulated.
 */
void
__wt_conn_foc_discard(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    size_t i;

    conn = S2C(session);

    /*
     * If we have a list of chunks to free, run through the list, then free the list itself.
     */
    for (i = 0; i < conn->foc_cnt; ++i)
        __wt_free(session, conn->foc[i]);
    __wt_free(session, conn->foc);
}

/*
 * __wt_configure_method --
 *     WT_CONNECTION.configure_method.
 */
int
__wt_configure_method(WT_SESSION_IMPL *session, const char *method, const char *uri,
  const char *config, const char *type, const char *check)
{
    WT_CONFIG_CHECK *checks, *newcheck;
    const WT_CONFIG_CHECK *cp;
    WT_CONFIG_ENTRY *entry;
    const WT_CONFIG_ENTRY **epp;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    size_t cnt, len;
    char *newcheck_name, *p;

    /*
     * !!!
     * We ignore the specified uri, that is, all new configuration options
     * will be valid for all data sources. That shouldn't be too bad as
     * the worst that can happen is an application might specify some
     * configuration option and not get an error -- the option should be
     * ignored by the underlying implementation since it's unexpected, so
     * there shouldn't be any real problems.  Eventually I expect we will
     * get the whole data-source thing sorted, at which time there may be
     * configuration arrays for each data source, and that's when the uri
     * will matter.
     */
    WT_UNUSED(uri);

    conn = S2C(session);
    checks = newcheck = NULL;
    entry = NULL;
    newcheck_name = NULL;

    /* Argument checking; we only support a limited number of types. */
    if (config == NULL)
        WT_RET_MSG(session, EINVAL, "no configuration specified");
    if (type == NULL)
        WT_RET_MSG(session, EINVAL, "no configuration type specified");
    if (strcmp(type, "boolean") != 0 && strcmp(type, "int") != 0 && strcmp(type, "list") != 0 &&
      strcmp(type, "string") != 0)
        WT_RET_MSG(
          session, EINVAL, "type must be one of \"boolean\", \"int\", \"list\" or \"string\"");

    /*
     * Translate the method name to our configuration names, then find a match.
     */
    for (epp = conn->config_entries; *epp != NULL && (*epp)->method != NULL; ++epp)
        if (strcmp((*epp)->method, method) == 0)
            break;
    if (*epp == NULL || (*epp)->method == NULL)
        WT_RET_MSG(session, WT_NOTFOUND, "no method matching %s found", method);

    /*
     * Technically possible for threads to race, lock the connection while adding the new
     * configuration information. We're holding the lock for an extended period of time, but
     * configuration changes should be rare and only happen during startup.
     */
    __wt_spin_lock(session, &conn->api_lock);

    /*
     * Allocate new configuration entry and fill it in.
     *
     * The new base value is the previous base value, a separator and the new configuration string.
     */
    WT_ERR(__wt_calloc_one(session, &entry));
    entry->method = (*epp)->method;
    len = strlen((*epp)->base) + strlen(",") + strlen(config) + 1;
    WT_ERR(__wt_calloc_def(session, len, &p));
    entry->base = p;
    WT_ERR(__wt_snprintf(p, len, "%s,%s", (*epp)->base, config));

    /*
     * There may be a default value in the config argument passed in (for example,
     * (kvs_parallelism=64"). The default value isn't part of the name, build a new one.
     */
    WT_ERR(__wt_strdup(session, config, &newcheck_name));
    if ((p = strchr(newcheck_name, '=')) != NULL)
        *p = '\0';

    /*
     * The new configuration name may replace an existing check with new information, in that case
     * skip the old version.
     */
    cnt = 0;
    if ((*epp)->checks != NULL)
        for (cp = (*epp)->checks; cp->name != NULL; ++cp)
            ++cnt;
    WT_ERR(__wt_calloc_def(session, cnt + 2, &checks));
    cnt = 0;
    if ((*epp)->checks != NULL)
        for (cp = (*epp)->checks; cp->name != NULL; ++cp)
            if (strcmp(newcheck_name, cp->name) != 0)
                checks[cnt++] = *cp;
    newcheck = &checks[cnt];
    newcheck->name = newcheck_name;
    WT_ERR(__wt_strdup(session, type, &newcheck->type));
    WT_ERR(__wt_strdup(session, check, &newcheck->checks));
    entry->checks = checks;
    entry->checks_entries = 0;

    /*
     * Confirm the configuration string passes the new set of checks.
     */
    WT_ERR(__wt_config_check(session, entry, config, 0));

    /*
     * The next time this configuration is updated, we don't want to figure out which of these
     * pieces of memory were allocated and will need to be free'd on close (this isn't a heavily
     * used API and it's too much work); add them all to the free-on-close list now. We don't check
     * for errors deliberately, we'd have to figure out which elements have already been added to
     * the free-on-close array and which have not in order to avoid freeing chunks of memory twice.
     * Again, this isn't a commonly used API and it shouldn't ever happen, just leak it.
     */
    __conn_foc_add(session, entry->base);
    __conn_foc_add(session, entry);
    __conn_foc_add(session, checks);
    __conn_foc_add(session, newcheck->type);
    __conn_foc_add(session, newcheck->checks);
    __conn_foc_add(session, newcheck_name);

    /*
     * Instead of using locks to protect configuration information, assume we can atomically update
     * a pointer to a chunk of memory, and because a pointer is never partially written, readers
     * will correctly see the original or new versions of the memory. Readers might be using the old
     * version as it's being updated, though, which means we cannot free the old chunk of memory
     * until all possible readers have finished. Currently, that's on connection close: in other
     * words, we can use this because it's small amounts of memory, and we really, really do not
     * want to acquire locks every time we access configuration strings, since that's done on every
     * API call.
     */
    WT_PUBLISH(*epp, entry);

    if (0) {
err:
        if (entry != NULL) {
            __wt_free(session, entry->base);
            __wt_free(session, entry);
        }
        __wt_free(session, checks);
        if (newcheck != NULL) {
            __wt_free(session, newcheck->type);
            __wt_free(session, newcheck->checks);
        }
        __wt_free(session, newcheck_name);
    }

    __wt_spin_unlock(session, &conn->api_lock);
    return (ret);
}
