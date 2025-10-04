/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conf_bind --
 *     Bind values to a configuration string.
 */
int
__wt_conf_bind(WT_SESSION_IMPL *session, const char *compiled_str, va_list ap)
{
    WT_CONF *conf;
    WT_CONFIG_ITEM *value;
    WT_CONF_BINDINGS *bound;
    WT_CONF_BIND_DESC *bind_desc;
    WT_CONNECTION_IMPL *conn;
    size_t len;
    uint64_t i;
    const char *str;

    conn = S2C(session);
    if (!__wt_conf_get_compiled(conn, compiled_str, &conf))
        return (EINVAL);

    bound = &session->conf_bindings;
    WT_CLEAR(*bound);

    for (i = 0; i < conf->binding_count; ++i) {
        bind_desc = conf->binding_descriptions[i];
        WT_ASSERT(session, i == bind_desc->offset);
        bound->values[i].desc = bind_desc;

        /* Fill in the bound value. */
        value = &bound->values[i].item;
        value->type = bind_desc->type;

        /*
         * We add a cast because we really want the default case, and some compilers would otherwise
         * not permit it.
         */
        switch ((u_int)bind_desc->type) {
        case WT_CONFIG_ITEM_NUM:
        case WT_CONFIG_ITEM_BOOL:
            /* The str/len fields will continue to be set to "%d" in our copy of the config string.
             */
            value->val = va_arg(ap, int64_t);
            break;
        case WT_CONFIG_ITEM_STRING:
        case WT_CONFIG_ITEM_ID:
            str = va_arg(ap, const char *);
            len = strlen(str);
            value->str = str;
            value->len = len;

            /*
             * Even when the bind format uses %s, we must check it against boolean constants. This
             * is done for the non-compiled cases and WiredTiger configuration processing code may
             * depend on it. We also change the string to point to fixed values of these constants
             * to have a consistent way to fast match strings that are part of choices. "false" and
             * "true" are legal as parts of a set of choices, and so can be used with
             * WT_CONF_STRING_MATCH. In addition, the value must be set, as the resulting
             * configuration item can be subsequently interpreted both as a boolean or as a string.
             */
            if (WT_STRING_LIT_MATCH("false", str, len)) {
                value->str = __WT_CONFIG_CHOICE_false;
                value->type = WT_CONFIG_ITEM_BOOL;
                value->val = 0;
            } else if (WT_STRING_LIT_MATCH("true", str, len)) {
                value->str = __WT_CONFIG_CHOICE_true;
                value->type = WT_CONFIG_ITEM_BOOL;
                value->val = 1;
            } else
                WT_RET(__wt_conf_check_choice(session, bind_desc->choices, str, len, &value->str));
            break;
        case WT_CONFIG_ITEM_STRUCT:
        default:
            return (__wt_illegal_value(session, bind_desc->type));
        }
    }

    return (0);
}
