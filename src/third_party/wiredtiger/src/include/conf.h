/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Sample usage:
 *  __wt_conf_gets(session, conf, statistics, &cval);
 *  __wt_conf_gets(session, conf, Operation_tracking.enabled, &cval);
 *  __wt_conf_gets_def(session, conf, no_timestamp, 0, &cval));
 */
#define __wt_conf_gets(s, conf, key, cval) \
    __wt_conf_gets_func(s, conf, WT_CONF_ID_STRUCTURE.key, 0, false, cval)

#define __wt_conf_gets_def(s, conf, key, def, cval) \
    __wt_conf_gets_def_func(s, conf, WT_CONF_ID_STRUCTURE.key, def, cval)

#define WT_CONF_BIND_VALUES_LEN 5

/*
 * WT_CONF_BINDINGS --
 *	A set of values bound.
 */
struct __wt_conf_bindings {
    struct {
        WT_CONF_BIND_DESC *desc;
        WT_CONFIG_ITEM item;
    } values[WT_CONF_BIND_VALUES_LEN];
};

/*
 * WT_CONF_BIND_DESC --
 *	A descriptor about a value to be bound.
 */
struct __wt_conf_bind_desc {
    WT_CONFIG_ITEM_TYPE type; /* WT_CONFIG_ITEM.type */
    const char **choices;     /* Choices for the value if applicable. */
    u_int offset;             /* offset into WT_SESSION::conf_bindings.values table */
};

/*
 * WT_CONF --
 *	A structure corresponding to a compiled configuration string.  This is set up to
 * be a "position-independent" structure, so once set up, it can be copied without having
 * to strdup any fields.
 *
 * We actually compile into a superstructure that has N WT_CONF struct in an array,
 * followed by M WT_CONF_VALUE structs in another array.  N is the maximum conf count,
 * M is the maximum key count, these values are stored in this structure.  These two
 * values are dependent on the particular API we are compiling, and whether the API has
 * sub-configurations and how many keys the API uses.
 *
 * When we copy a conf (for example, by taking a precompiled configuration and making a
 * copy so we can add modifications), we must copy the entire superstructure.
 */
struct __wt_conf {
    uint8_t bitmap_default[__bitstr_size(WT_CONF_ID_COUNT)];

    const WT_CONFIG_ENTRY *compile_time_entry; /* May be used for diagnostic checks. */

    /*
     * These strings are in this struct because they can be valuable during debugging. They are not
     * otherwise used by the conf sources. The source configuration is owned by this struct and will
     * be freed when the struct is freed. The others are not owned, but they are guaranteed to be
     * valid throughout the lifetime of this struct.
     */
    char *source_config;        /* Source string used to pre-compile this struct */
    const char *api_config;     /* String from an API call to compile this struct */
    const char *default_config; /* Default string at the time of the compilation */

    uint8_t value_map[WT_CONF_ID_COUNT]; /* For each key, a 1-based index into conf_value */

    uint32_t conf_count;
    uint32_t conf_max;

    uint32_t conf_value_count;
    uint32_t conf_value_max;

    size_t conf_value_table_offset;

    uint32_t binding_count;
    size_t binding_allocated;
    WT_CONF_BIND_DESC **binding_descriptions;
};

/*
 * To keep the conf structure position independent, we cannot have a direct pointer to the key
 * table, we must deduce its position.
 */
#define WT_CONF_VALUE_TABLE_ENTRY(conf, n) \
    &((WT_CONF_VALUE *)((uint8_t *)conf + conf->conf_value_table_offset))[n]

struct __wt_conf_value {
    enum {
        CONF_VALUE_DEFAULT_ITEM,
        CONF_VALUE_NONDEFAULT_ITEM,
        CONF_VALUE_BIND_DESC,
        CONF_VALUE_SUB_INFO
    } type;
    union {
        WT_CONFIG_ITEM item;
        WT_CONF_BIND_DESC bind_desc;
        u_int sub_conf_index;
    } u;
};

#define WT_SIZEOF_FIELD(t, f) (sizeof(((t *)0)->f))
#define WT_FIELD_ELEMENTS(t, f) (WT_SIZEOF_FIELD(t, f) / WT_SIZEOF_FIELD(t, f[0]))

#define WT_CONF_API_TYPE(c, m) struct __wt_conf_api_##c##_##m
#define WT_CONF_API_DECLARE(c, m, nconf, nitem) \
    WT_CONF_API_TYPE(c, m)                      \
    {                                           \
        WT_CONF conf[nconf];                    \
        WT_CONF_VALUE conf_value[nitem];        \
    }

#define WT_CONF_DEFAULT_VALUE_SHORTCUT(conf, keys) \
    ((keys) < WT_CONF_ID_COUNT && __bit_test(&(conf)->bitmap_default[0], (keys)))

#define WT_CONF_API_COUNT(c, m) (WT_FIELD_ELEMENTS(WT_CONF_API_TYPE(c, m), conf))
#define WT_CONF_API_VALUE_COUNT(c, m) (WT_FIELD_ELEMENTS(WT_CONF_API_TYPE(c, m), conf_value))

#define WT_CONF_SIZING_INITIALIZE(c, m) \
    sizeof(WT_CONF_API_TYPE(c, m)), WT_CONF_API_COUNT(c, m), WT_CONF_API_VALUE_COUNT(c, m)

#define WT_CONF_SIZING_NONE 0, 0, 0

#define WT_CONF_STRING_MATCH(name, cval) ((cval).str == __WT_CONFIG_CHOICE_##name)

/*
 * DO NOT EDIT: automatically built by dist/api_config.py.
 * Per-API configuration structure declarations: BEGIN
 */
WT_CONF_API_DECLARE(WT_CONNECTION, close, 1, 3);
WT_CONF_API_DECLARE(WT_CONNECTION, debug_info, 1, 7);
WT_CONF_API_DECLARE(WT_CONNECTION, load_extension, 1, 4);
WT_CONF_API_DECLARE(WT_CONNECTION, open_session, 3, 9);
WT_CONF_API_DECLARE(WT_CONNECTION, query_timestamp, 1, 1);
WT_CONF_API_DECLARE(WT_CONNECTION, reconfigure, 16, 94);
WT_CONF_API_DECLARE(WT_CONNECTION, rollback_to_stable, 1, 2);
WT_CONF_API_DECLARE(WT_CONNECTION, set_timestamp, 1, 4);
WT_CONF_API_DECLARE(WT_CURSOR, bound, 1, 3);
WT_CONF_API_DECLARE(WT_CURSOR, reconfigure, 1, 3);
WT_CONF_API_DECLARE(WT_SESSION, alter, 3, 16);
WT_CONF_API_DECLARE(WT_SESSION, begin_transaction, 2, 11);
WT_CONF_API_DECLARE(WT_SESSION, checkpoint, 2, 10);
WT_CONF_API_DECLARE(WT_SESSION, commit_transaction, 1, 4);
WT_CONF_API_DECLARE(WT_SESSION, compact, 1, 5);
WT_CONF_API_DECLARE(WT_SESSION, create, 8, 83);
WT_CONF_API_DECLARE(WT_SESSION, drop, 1, 5);
WT_CONF_API_DECLARE(WT_SESSION, join, 1, 7);
WT_CONF_API_DECLARE(WT_SESSION, log_flush, 1, 1);
WT_CONF_API_DECLARE(WT_SESSION, open_cursor, 3, 29);
WT_CONF_API_DECLARE(WT_SESSION, prepare_transaction, 1, 1);
WT_CONF_API_DECLARE(WT_SESSION, query_timestamp, 1, 1);
WT_CONF_API_DECLARE(WT_SESSION, reconfigure, 3, 9);
WT_CONF_API_DECLARE(WT_SESSION, rollback_transaction, 1, 1);
WT_CONF_API_DECLARE(WT_SESSION, salvage, 1, 1);
WT_CONF_API_DECLARE(WT_SESSION, timestamp_transaction, 1, 4);
WT_CONF_API_DECLARE(WT_SESSION, verify, 1, 11);
WT_CONF_API_DECLARE(colgroup, meta, 2, 12);
WT_CONF_API_DECLARE(file, config, 5, 55);
WT_CONF_API_DECLARE(file, meta, 5, 62);
WT_CONF_API_DECLARE(index, meta, 2, 17);
WT_CONF_API_DECLARE(lsm, meta, 7, 74);
WT_CONF_API_DECLARE(object, meta, 5, 64);
WT_CONF_API_DECLARE(table, meta, 2, 13);
WT_CONF_API_DECLARE(tier, meta, 5, 65);
WT_CONF_API_DECLARE(tiered, meta, 5, 67);
WT_CONF_API_DECLARE(GLOBAL, wiredtiger_open, 20, 157);
WT_CONF_API_DECLARE(GLOBAL, wiredtiger_open_all, 20, 158);
WT_CONF_API_DECLARE(GLOBAL, wiredtiger_open_basecfg, 20, 152);
WT_CONF_API_DECLARE(GLOBAL, wiredtiger_open_usercfg, 20, 151);

#define WT_CONF_API_ELEMENTS 57

/*
 * Per-API configuration structure declarations: END
 */
