/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conf_gets_func --
 *     Given a compiled structure of configuration strings, find the final value for a given key,
 *     represented as (up to 4) 16-bit key ids packed into a 64-bit key. If a default is given, it
 *     overrides any default found in the compiled structure.
 */
int
__wt_conf_gets_func(WT_SESSION_IMPL *session, const WT_CONF *orig_conf, uint64_t orig_keys, int def,
  bool use_def, WT_CONFIG_ITEM *value)
{
    WT_CONFIG_ITEM_STATIC_INIT(false_value);
    WT_CONF_BIND_DESC *bind_desc;
    WT_CONF_VALUE *conf_value;
    const WT_CONF *conf;
    uint64_t keys;
    uint32_t partkey, values_off;
    uint8_t conf_value_index;

    conf = orig_conf;
    keys = orig_keys;

    WT_ASSERT(session, keys != 0);
    while (keys != 0) {
        partkey = keys & 0xffff;
        WT_ASSERT(session, partkey != 0 && partkey < WT_CONF_ID_COUNT);

        conf_value_index = conf->value_map[partkey];
        if (conf_value_index == 0)
            return (WT_NOTFOUND);

        /* The value in value_map is one-based, account for that here. */
        --conf_value_index;
        WT_ASSERT(session, conf_value_index < conf->conf_value_count);
        conf_value = WT_CONF_VALUE_TABLE_ENTRY(conf, conf_value_index);

        /* Set up a new value of keys for use if we have a sub-configuration. */
        keys >>= 16;

        switch (conf_value->type) {
        case CONF_VALUE_DEFAULT_ITEM:
            if (use_def) {
                *value = false_value;
                value->val = def;
                return (0);
            }

        /* FALLTHROUGH */
        case CONF_VALUE_NONDEFAULT_ITEM:
            if (keys != 0)
                return (WT_NOTFOUND);
            *value = conf_value->u.item;
            return (0);

        case CONF_VALUE_BIND_DESC:
            if (keys != 0)
                return (WT_NOTFOUND);
            bind_desc = &conf_value->u.bind_desc;
            values_off = bind_desc->offset;
            WT_ASSERT(session,
              bind_desc->offset < orig_conf->binding_count &&
                values_off <= WT_CONF_BIND_VALUES_LEN);
            if (session->conf_bindings.values[values_off].desc != bind_desc)
                WT_RET_MSG(session, EINVAL,
                  "configuration value(s) have not been bound with bind_configuration");
            *value = session->conf_bindings.values[values_off].item;
            return (0);

        case CONF_VALUE_SUB_INFO:
            conf = &conf[conf_value->u.sub_conf_index];

            /* Do a quick check to see if the sub-configuration value is the default. */
            if (use_def && WT_CONF_DEFAULT_VALUE_SHORTCUT(conf, keys & 0xffff)) {
                *value = false_value;
                value->val = def;
                return (0);
            }
            break;
        }
    }
    return (WT_NOTFOUND);
}
