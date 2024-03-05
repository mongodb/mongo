/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_conf_get_compiled --
 *     Return true if and only if the given string is a dummy compiled string, and if so, return the
 *     compiled structure.
 */
static WT_INLINE bool
__wt_conf_get_compiled(WT_CONNECTION_IMPL *conn, const char *config, WT_CONF **confp)
{
    if (!__wt_conf_is_compiled(conn, config))
        return (false);

    *confp = conn->conf_array[(uint32_t)(config - conn->conf_dummy)];
    return (true);
}

/*
 * __wt_conf_is_compiled --
 *     Return true if and only if the given string is a dummy compiled string.
 */
static WT_INLINE bool
__wt_conf_is_compiled(WT_CONNECTION_IMPL *conn, const char *config)
{
    return (config >= conn->conf_dummy && config < conn->conf_dummy + conn->conf_size);
}

/*
 * __wt_conf_check_choice --
 *     Check the string value against a list of choices, if it is found, set up the value so it can
 *     be checked against a particular choice quickly.
 */
static WT_INLINE int
__wt_conf_check_choice(
  WT_SESSION_IMPL *session, const char **choices, const char *str, size_t len, const char **result)
{
    const char *choice;

    if (choices == NULL)
        return (0);

    /*
     * Find the choice, and set the string in the value to the entry in the choice table. It's the
     * same string, but the address is known by an external identifier (e.g.
     * __WT_CONFIG_CHOICE_force). That way it can be checked without a string compare call by using
     * the WT_CONF_STRING_MATCH macro.
     */
    for (; (choice = *choices) != NULL; ++choices)
        if (WT_STRING_MATCH(choice, str, len)) {
            *result = choice;
            break;
        }

    if (choice == NULL) {
        /*
         * We didn't find it in the list of choices. It's legal to specify a choice as blank, we
         * have a special value to indicate that. We check this last because this is a rare case,
         * especially when we are binding a parameter, which is the fast path we optimize for.
         */
        if (len == 0)
            *result = __WT_CONFIG_CHOICE_NULL;
        else
            WT_RET_MSG(session, EINVAL, "Value '%.*s' is not a valid choice", (int)len, str);
    }
    return (0);
}

/*
 * __wt_conf_check_one --
 *     Do all configuration checks for a single value.
 */
static WT_INLINE int
__wt_conf_check_one(WT_SESSION_IMPL *session, const WT_CONFIG_CHECK *check, WT_CONFIG_ITEM *value)
{
    if (check->checkf != NULL)
        WT_RET(check->checkf(session, value));

    /*
     * If the checks string is empty, there are no additional checks we need to make. This is the
     * only point we need the checks string, as the information for checking is also in the checks
     * structure.
     */
    if (check->checks != NULL) {

        /*
         * If it must be one of a choice of strings, check that now.
         */
        WT_RET(
          __wt_conf_check_choice(session, check->choices, value->str, value->len, &value->str));

        if (value->val < check->min_value)
            WT_RET_MSG(session, EINVAL, "Value '%.*s' too small, the minimum is %" PRIi64,
              (int)value->len, value->str, check->min_value);

        if (value->val > check->max_value)
            WT_RET_MSG(session, EINVAL, "Value '%.*s' too large, the maximum is %" PRIi64,
              (int)value->len, value->str, check->max_value);
    }
    return (0);
}

/*
 * __wt_conf_gets_def_func --
 *     Get a value from the compiled configuration. If the value is a default, return that.
 */
static WT_INLINE int
__wt_conf_gets_def_func(
  WT_SESSION_IMPL *session, const WT_CONF *conf, uint64_t keys, int def, WT_CONFIG_ITEM *value)
{
    WT_CONFIG_ITEM_STATIC_INIT(false_value);

    /*
     * A shortcut - if the value in the compiled config is a default, return the default that the
     * caller gave.
     */
    if (WT_CONF_DEFAULT_VALUE_SHORTCUT(conf, keys & 0xffff)) {
        *value = false_value;
        value->val = def;
        return (0);
    }
    return (__wt_conf_gets_func(session, conf, keys, def, true, value));
}
