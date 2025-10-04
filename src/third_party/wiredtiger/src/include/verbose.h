/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once
/*
 * Structure to bundle verbose message identification details.
 */
struct __wt_verbose_message_info {
    uint32_t id;
    WT_VERBOSE_CATEGORY category;
    WT_VERBOSE_LEVEL level;
};

/* clang-format off */
#define WT_VERBOSE_CATEGORY_STR_INIT \
    { \
    /* AUTOMATIC VERBOSE ENUM STRING GENERATION START */ \
    "WT_VERB_ALL", \
    "WT_VERB_API", \
    "WT_VERB_BACKUP", \
    "WT_VERB_BLKCACHE", \
    "WT_VERB_BLOCK", \
    "WT_VERB_CHECKPOINT", \
    "WT_VERB_CHECKPOINT_CLEANUP", \
    "WT_VERB_CHECKPOINT_PROGRESS", \
    "WT_VERB_CHUNKCACHE", \
    "WT_VERB_COMPACT", \
    "WT_VERB_COMPACT_PROGRESS", \
    "WT_VERB_CONFIGURATION", \
    "WT_VERB_DEFAULT", \
    "WT_VERB_DISAGGREGATED_STORAGE", \
    "WT_VERB_ERROR_RETURNS", \
    "WT_VERB_EVICTION", \
    "WT_VERB_EXTENSION", \
    "WT_VERB_FILEOPS", \
    "WT_VERB_GENERATION", \
    "WT_VERB_HANDLEOPS", \
    "WT_VERB_HS", \
    "WT_VERB_HS_ACTIVITY", \
    "WT_VERB_LAYERED", \
    "WT_VERB_LIVE_RESTORE", \
    "WT_VERB_LIVE_RESTORE_PROGRESS", \
    "WT_VERB_LOG", \
    "WT_VERB_MUTEX", \
    "WT_VERB_METADATA", \
    "WT_VERB_OUT_OF_ORDER", \
    "WT_VERB_OVERFLOW", \
    "WT_VERB_PAGE_DELTA", \
    "WT_VERB_PREFETCH", \
    "WT_VERB_READ", \
    "WT_VERB_RECONCILE", \
    "WT_VERB_RECOVERY", \
    "WT_VERB_RECOVERY_PROGRESS", \
    "WT_VERB_RTS", \
    "WT_VERB_SALVAGE", \
    "WT_VERB_SHARED_CACHE", \
    "WT_VERB_SPLIT", \
    "WT_VERB_SWEEP", \
    "WT_VERB_TEMPORARY", \
    "WT_VERB_THREAD_GROUP", \
    "WT_VERB_TIERED", \
    "WT_VERB_TIMESTAMP", \
    "WT_VERB_TRANSACTION", \
    "WT_VERB_VERIFY", \
    "WT_VERB_VERSION", \
    "WT_VERB_WRITE", \
    /* AUTOMATIC VERBOSE ENUM STRING GENERATION STOP */ \
    }
/* clang-format on */

#define WT_DEFAULT_LOG_ID 1000000 /* Default log ID 1000000 for verbose messages */

/* Convert a verbose level to its string representation. */
#define WT_VERBOSE_LEVEL_STR(level, level_str) \
    do {                                       \
        (level_str) = "";                      \
        switch (level) {                       \
        case WT_VERBOSE_ERROR:                 \
            (level_str) = "ERROR";             \
            break;                             \
        case WT_VERBOSE_WARNING:               \
            (level_str) = "WARNING";           \
            break;                             \
        case WT_VERBOSE_NOTICE:                \
            (level_str) = "NOTICE";            \
            break;                             \
        case WT_VERBOSE_INFO:                  \
            (level_str) = "INFO";              \
            break;                             \
        case WT_VERBOSE_DEBUG_1:               \
            (level_str) = "DEBUG_1";           \
            break;                             \
        case WT_VERBOSE_DEBUG_2:               \
            (level_str) = "DEBUG_2";           \
            break;                             \
        case WT_VERBOSE_DEBUG_3:               \
            (level_str) = "DEBUG_3";           \
            break;                             \
        case WT_VERBOSE_DEBUG_4:               \
            (level_str) = "DEBUG_4";           \
            break;                             \
        case WT_VERBOSE_DEBUG_5:               \
            (level_str) = "DEBUG_5";           \
            break;                             \
        }                                      \
    } while (0)

/*
 * Default verbosity level. WT_VERBOSE_DEBUG_1 being the default level assigned to verbose messages
 * prior to the introduction of verbosity levels.
 */
#ifndef WT_VERBOSE_LEVEL_DEFAULT
#define WT_VERBOSE_LEVEL_DEFAULT WT_VERBOSE_DEBUG_1
#endif

/* Default category for messages that don't explicitly specify a category. */
#ifndef WT_VERBOSE_CATEGORY_DEFAULT
#define WT_VERBOSE_CATEGORY_DEFAULT WT_VERB_DEFAULT
#endif

/*
 * WT_VERBOSE_MULTI_CATEGORY --
 *  Simple structure to represent a set of verbose categories.
 */
struct __wt_verbose_multi_category {
    WT_VERBOSE_CATEGORY *categories;
    uint32_t cnt;
};

/* Generate a set of verbose categories. */
#define WT_DECL_VERBOSE_MULTI_CATEGORY(items) \
    ((WT_VERBOSE_MULTI_CATEGORY){.categories = (items), .cnt = WT_ELEMENTS(items)})

/* Set the verbosity level for a given category. */
#define WT_SET_VERBOSE_LEVEL(session, category, level) S2C(session)->verbose[category] = level;

/* Check if a given verbosity level satisfies the verbosity level of a category. */
#define WT_VERBOSE_LEVEL_ISSET(session, category, level) (level <= S2C(session)->verbose[category])

/*
 * Given this verbosity check is without an explicit verbosity level, the macro checks whether the
 * given category satisfies the default verbosity level.
 */
#define WT_VERBOSE_ISSET(session, category) \
    WT_VERBOSE_LEVEL_ISSET(session, category, WT_VERBOSE_LEVEL_DEFAULT)

/* Set the verbose level and save the previous value. */
#define WT_VERBOSE_SET_AND_SAVE(session, verbose_orig_level, category, level) \
    do {                                                                      \
        verbose_orig_level[category] = S2C(session)->verbose[category];       \
        WT_SET_VERBOSE_LEVEL(session, category, level);                       \
    } while (0)

/* Restore the original level  */
#define WT_VERBOSE_RESTORE(session, verbose_orig_level, category)              \
    do {                                                                       \
        WT_SET_VERBOSE_LEVEL(session, category, verbose_orig_level[category]); \
    } while (0)

/*
 * __wt_verbose_level --
 *     Display a verbose message considering a category and a verbosity level.
 */
#define __wt_verbose_level(session, category, level, fmt, ...)               \
    do {                                                                     \
        if (WT_VERBOSE_LEVEL_ISSET(session, category, level))                \
            __wt_verbose_worker(session, category, level, fmt, __VA_ARGS__); \
    } while (0)

/*
 * __wt_verbose_error --
 *     Wrapper to __wt_verbose_level defaulting the verbosity level to WT_VERBOSE_ERROR.
 */
#define __wt_verbose_error(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_ERROR, fmt, __VA_ARGS__)

/*
 * __wt_verbose_warning --
 *     Wrapper to __wt_verbose_level defaulting the verbosity level to WT_VERBOSE_WARNING.
 */
#define __wt_verbose_warning(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_WARNING, fmt, __VA_ARGS__)

/*
 * __wt_verbose_notice --
 *     Wrapper to __wt_verbose_level defaulting the verbosity level to WT_VERBOSE_NOTICE.
 */
#define __wt_verbose_notice(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_NOTICE, fmt, __VA_ARGS__)

/*
 * __wt_verbose_info --
 *     Wrapper to __wt_verbose_level defaulting the verbosity level to WT_VERBOSE_INFO.
 */
#define __wt_verbose_info(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_INFO, fmt, __VA_ARGS__)

/*
 * __wt_verbose_level_id --
 *     Check for the verbosity level and invoke the worker with an id.
 */
#define __wt_verbose_level_id(session, log_id, verb_category, verb_level, fmt, ...)      \
    do {                                                                                 \
        if (WT_VERBOSE_LEVEL_ISSET((session), verb_category, verb_level)) {              \
            WT_VERBOSE_MESSAGE_INFO verb_message_info = {                                \
              .id = log_id, .category = verb_category, .level = verb_level};             \
            __wt_verbose_worker_id((session), (&verb_message_info), (fmt), __VA_ARGS__); \
        }                                                                                \
    } while (0)

/*
 * __wt_verbose_info_id --
 *     Wrapper to __wt_verbose_level_id defaulting the verbosity level to WT_VERBOSE_INFO with a log
 *     id.
 */
#define __wt_verbose_info_id(session, log_id, category, fmt, ...) \
    __wt_verbose_level_id(session, log_id, category, WT_VERBOSE_INFO, fmt, __VA_ARGS__)

/*
 * __wt_verbose_debug1 --
 *     Wrapper to __wt_verbose_level using the default (DEBUG_1) verbosity level.
 */
#define __wt_verbose_debug1(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_DEBUG_1, fmt, __VA_ARGS__)

/*
 * __wt_verbose_debug2 --
 *     Wrapper to __wt_verbose_level using the DEBUG_2 level.
 */
#define __wt_verbose_debug2(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_DEBUG_2, fmt, __VA_ARGS__)

/*
 * __wt_verbose_debug3 --
 *     Wrapper to __wt_verbose_level using the DEBUG_3 level.
 */
#define __wt_verbose_debug3(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_DEBUG_3, fmt, __VA_ARGS__)

/*
 * __wt_verbose --
 *     Display a verbose message using the default verbosity level. Not an inlined function because
 *     you can't inline functions taking variadic arguments and we don't want to make a function
 *     call in production systems just to find out a verbose flag isn't set. The macro must take a
 *     format string and at least one additional argument, there's no portable way to remove the
 *     comma before an empty __VA_ARGS__ value.
 */
#define __wt_verbose(session, category, fmt, ...) \
    __wt_verbose_level(session, category, WT_VERBOSE_LEVEL_DEFAULT, fmt, __VA_ARGS__)

/*
 * __wt_verbose_level_multi_id --
 *     Refer to __wt_verbose_level_multi for details.
 */
#define __wt_verbose_level_multi_id(session, log_id, multi_category, level, fmt, ...)          \
    do {                                                                                       \
        uint32_t __v_idx;                                                                      \
        WT_VERBOSE_MULTI_CATEGORY __multi_category = multi_category;                           \
        for (__v_idx = 0; __v_idx < __multi_category.cnt; __v_idx++) {                         \
            __wt_verbose_level_id(                                                             \
              session, log_id, __multi_category.categories[__v_idx], level, fmt, __VA_ARGS__); \
        }                                                                                      \
    } while (0)

/*
 * __wt_verbose_level_multi --
 *     Display a verbose message, given a set of multiple verbose categories. A verbose message will
 *     be displayed if at least one category in the set satisfies the required verbosity level.
 */
#define __wt_verbose_level_multi(session, multi_category, level, fmt, ...)                        \
    do {                                                                                          \
        uint32_t __v_idx;                                                                         \
        /*                                                                                        \
         * multi_category can be a ternary expression that returns one of two structs. If we call \
         * it 3 times in this macro then we're evaluating that ternary 3 times and could return a \
         * different value on a second call. Save it into a local variable to make sure we're     \
         * working with a constant value.                                                         \
         */                                                                                       \
        WT_VERBOSE_MULTI_CATEGORY __multi_category = multi_category;                              \
        for (__v_idx = 0; __v_idx < __multi_category.cnt; __v_idx++) {                            \
            if (WT_VERBOSE_LEVEL_ISSET(session, __multi_category.categories[__v_idx], level)) {   \
                __wt_verbose_worker(                                                              \
                  session, __multi_category.categories[__v_idx], level, fmt, __VA_ARGS__);        \
                break;                                                                            \
            }                                                                                     \
        }                                                                                         \
    } while (0)

/*
 * __wt_verbose_multi --
 *     Display a verbose message, given a set of multiple verbose categories using the default
 *     verbosity level.
 */
#define __wt_verbose_multi(session, multi_category, fmt, ...)                                     \
    do {                                                                                          \
        uint32_t __v_idx;                                                                         \
        /*                                                                                        \
         * multi_category can be a ternary expression that returns one of two structs. If we call \
         * it 3 times in this macro then we're evaluating that ternary 3 times and could return a \
         * different value on a second call. Save it into a local variable to make sure we're     \
         * working with a constant value.                                                         \
         */                                                                                       \
        WT_VERBOSE_MULTI_CATEGORY __multi_category = multi_category;                              \
        for (__v_idx = 0; __v_idx < __multi_category.cnt; __v_idx++) {                            \
            if (WT_VERBOSE_ISSET(session, __multi_category.categories[__v_idx])) {                \
                __wt_verbose_worker(session, __multi_category.categories[__v_idx],                \
                  WT_VERBOSE_LEVEL_DEFAULT, fmt, __VA_ARGS__);                                    \
                break;                                                                            \
            }                                                                                     \
        }                                                                                         \
    } while (0)
