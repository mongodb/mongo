/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifdef HAVE_DIAGNOSTIC
/*
 * Capture cases where a single session handle is used by multiple threads in parallel. The check
 * isn't trivial because some API calls re-enter via public API entry points and the session with ID
 * 0 is the default session in the connection handle which can be used across multiple threads. It
 * is safe to use the reference count without atomic operations because the reference count is only
 * tracking a thread re-entering the API.
 */
#define WT_SINGLE_THREAD_CHECK_START(s)                                                   \
    {                                                                                     \
        uintmax_t __tmp_api_tid;                                                          \
        __wt_thread_id(&__tmp_api_tid);                                                   \
        WT_ASSERT(s, (s)->id == 0 || (s)->api_tid == 0 || (s)->api_tid == __tmp_api_tid); \
        if ((s)->api_tid == 0) {                                                          \
            (s)->api_tid = __tmp_api_tid;                                                 \
            WT_FULL_BARRIER();                                                            \
        }                                                                                 \
        ++(s)->api_enter_refcnt;                                                          \
    }

#define WT_SINGLE_THREAD_CHECK_STOP(s) \
    if (--(s)->api_enter_refcnt == 0)  \
        WT_PUBLISH((s)->api_tid, 0);
#else
#define WT_SINGLE_THREAD_CHECK_START(s)
#define WT_SINGLE_THREAD_CHECK_STOP(s)
#endif

#define API_SESSION_PUSH(s, h, n, dh)                                                        \
    WT_DATA_HANDLE *__olddh = (s)->dhandle;                                                  \
    const char *__oldname;                                                                   \
    /* If this isn't an API reentry, the name should be NULL and the counter should be 0. */ \
    WT_ASSERT(s, (s)->name != NULL || (s)->api_call_counter == 0);                           \
    __oldname = (s)->name;                                                                   \
    ++(s)->api_call_counter;                                                                 \
    (s)->dhandle = (dh);                                                                     \
    (s)->name = (s)->lastop = #h "." #n
#define API_SESSION_POP(s)  \
    (s)->dhandle = __olddh; \
    (s)->name = __oldname;  \
    --(s)->api_call_counter

/* Standard entry points to the API: declares/initializes local variables. */
#define API_SESSION_INIT(s, h, n, dh)                                   \
    WT_TRACK_OP_DECL;                                                   \
    API_SESSION_PUSH(s, h, n, dh);                                      \
    /*                                                                  \
     * No code before this line, otherwise error handling won't be      \
     * correct.                                                         \
     */                                                                 \
    WT_ERR(WT_SESSION_CHECK_PANIC(s));                                  \
    WT_SINGLE_THREAD_CHECK_START(s);                                    \
    WT_TRACK_OP_INIT(s);                                                \
    if ((s)->api_call_counter == 1 && !F_ISSET(s, WT_SESSION_INTERNAL)) \
        __wt_op_timer_start(s);                                         \
    /* Reset wait time if this isn't an API reentry. */                 \
    if ((s)->api_call_counter == 1)                                     \
        (s)->cache_wait_us = 0;                                         \
    __wt_verbose((s), WT_VERB_API, "%s", "CALL: " #h ":" #n)

#define API_CALL_NOCONF(s, h, n, dh) \
    do {                             \
    API_SESSION_INIT(s, h, n, dh)

#define API_CALL(s, h, n, dh, config, cfg)                                \
    do {                                                                  \
        const char *(cfg)[] = {WT_CONFIG_BASE(s, h##_##n), config, NULL}; \
        API_SESSION_INIT(s, h, n, dh);                                    \
        if ((config) != NULL)                                             \
    WT_ERR(__wt_config_check((s), WT_CONFIG_REF(s, h##_##n), (config), 0))

#define API_END(s, ret)                                                                    \
    if ((s) != NULL) {                                                                     \
        WT_TRACK_OP_END(s);                                                                \
        WT_SINGLE_THREAD_CHECK_STOP(s);                                                    \
        if ((ret) != 0)                                                                    \
            __wt_txn_err_set(s, ret);                                                      \
        if ((s)->api_call_counter == 1 && !F_ISSET(s, WT_SESSION_INTERNAL))                \
            __wt_op_timer_stop(s);                                                         \
        /*                                                                                 \
         * We should not leave any history store cursor open when return from an api call. \
         * However, we cannot do a stricter check before WT-7247 is resolved.              \
         */                                                                                \
        WT_ASSERT(s, (s)->api_call_counter > 1 || (s)->hs_cursor_counter <= 3);            \
        /*                                                                                 \
         * No code after this line, otherwise error handling                               \
         * won't be correct.                                                               \
         */                                                                                \
        API_SESSION_POP(s);                                                                \
    }                                                                                      \
    }                                                                                      \
    while (0)

/* An API call wrapped in a transaction if necessary. */
#define TXN_API_CALL(s, h, n, dh, config, cfg)                              \
    do {                                                                    \
        bool __autotxn = false, __update = false;                           \
        API_CALL(s, h, n, dh, config, cfg);                                 \
        __wt_txn_timestamp_flags(s);                                        \
        __autotxn = !F_ISSET((s)->txn, WT_TXN_AUTOCOMMIT | WT_TXN_RUNNING); \
        if (__autotxn)                                                      \
            F_SET((s)->txn, WT_TXN_AUTOCOMMIT);                             \
        __update = !F_ISSET((s)->txn, WT_TXN_UPDATE);                       \
        if (__update)                                                       \
            F_SET((s)->txn, WT_TXN_UPDATE);

/* An API call wrapped in a transaction if necessary. */
#define TXN_API_CALL_NOCONF(s, h, n, dh)                                    \
    do {                                                                    \
        bool __autotxn = false, __update = false;                           \
        API_CALL_NOCONF(s, h, n, dh);                                       \
        __wt_txn_timestamp_flags(s);                                        \
        __autotxn = !F_ISSET((s)->txn, WT_TXN_AUTOCOMMIT | WT_TXN_RUNNING); \
        if (__autotxn)                                                      \
            F_SET((s)->txn, WT_TXN_AUTOCOMMIT);                             \
        __update = !F_ISSET((s)->txn, WT_TXN_UPDATE);                       \
        if (__update)                                                       \
            F_SET((s)->txn, WT_TXN_UPDATE);

/* End a transactional API call, optional retry on rollback. */
#define TXN_API_END(s, ret, retry)                         \
    API_END(s, ret);                                       \
    if (__update)                                          \
        F_CLR((s)->txn, WT_TXN_UPDATE);                    \
    if (__autotxn) {                                       \
        if (F_ISSET((s)->txn, WT_TXN_AUTOCOMMIT))          \
            F_CLR((s)->txn, WT_TXN_AUTOCOMMIT);            \
        else if ((ret) == 0)                               \
            (ret) = __wt_txn_commit((s), NULL);            \
        else {                                             \
            if (retry)                                     \
                WT_TRET(__wt_session_copy_values(s));      \
            WT_TRET(__wt_txn_rollback((s), NULL));         \
            if ((retry) && (ret) == WT_ROLLBACK) {         \
                (ret) = 0;                                 \
                continue;                                  \
            }                                              \
            WT_TRET(__wt_session_reset_cursors(s, false)); \
        }                                                  \
    }                                                      \
    break;                                                 \
    }                                                      \
    while (1)

/*
 * In almost all cases, API_END is returning immediately, make it simple. If a session or connection
 * method is about to return WT_NOTFOUND (some underlying object was not found), map it to ENOENT,
 * only cursor methods return WT_NOTFOUND.
 */
#define API_END_RET(s, ret) \
    API_END(s, ret);        \
    return (ret)
#define API_END_RET_NOTFOUND_MAP(s, ret) \
    API_END(s, ret);                     \
    return ((ret) == WT_NOTFOUND ? ENOENT : (ret))

/*
 * Used in cases where transaction error should not be set, but the error is returned from the API.
 * Success is passed to the API_END macro. If the method is about to return WT_NOTFOUND map it to
 * ENOENT.
 */
#define API_END_RET_NO_TXN_ERROR(s, ret) \
    API_END(s, 0);                       \
    return ((ret) == WT_NOTFOUND ? ENOENT : (ret))

#define CONNECTION_API_CALL(conn, s, n, config, cfg) \
    s = (conn)->default_session;                     \
    API_CALL(s, WT_CONNECTION, n, NULL, config, cfg)

#define CONNECTION_API_CALL_NOCONF(conn, s, n) \
    s = (conn)->default_session;               \
    API_CALL_NOCONF(s, WT_CONNECTION, n, NULL)

#define SESSION_API_CALL_PREPARE_ALLOWED(s, n, config, cfg) \
    API_CALL(s, WT_SESSION, n, NULL, config, cfg)

#define SESSION_API_CALL_PREPARE_ALLOWED_NOCONF(s, n) API_CALL_NOCONF(s, WT_SESSION, n, NULL)

#define SESSION_API_CALL_PREPARE_NOT_ALLOWED(s, n, config, cfg) \
    SESSION_API_PREPARE_CHECK(s, WT_SESSION, n);                \
    API_CALL(s, WT_SESSION, n, NULL, config, cfg)

#define SESSION_API_CALL_PREPARE_NOT_ALLOWED_NOCONF(s, n) \
    SESSION_API_PREPARE_CHECK(s, WT_SESSION, n);          \
    API_CALL_NOCONF(s, WT_SESSION, n, NULL)

#define SESSION_API_PREPARE_CHECK(s, h, n)                     \
    do {                                                       \
        if ((s)->api_call_counter == 0) {                      \
            int __prepare_ret;                                 \
            API_SESSION_PUSH(s, WT_SESSION, n, NULL);          \
            __prepare_ret = __wt_txn_context_prepare_check(s); \
            API_SESSION_POP(s);                                \
            WT_RET(__prepare_ret);                             \
        }                                                      \
    } while (0)

#define SESSION_API_CALL(s, n, config, cfg)      \
    SESSION_API_PREPARE_CHECK(s, WT_SESSION, n); \
    API_CALL(s, WT_SESSION, n, NULL, config, cfg)

#define SESSION_API_CALL_NOCONF(s, n) API_CALL_NOCONF(s, WT_SESSION, n, NULL)

#define SESSION_TXN_API_CALL(s, n, config, cfg)  \
    SESSION_API_PREPARE_CHECK(s, WT_SESSION, n); \
    TXN_API_CALL(s, WT_SESSION, n, NULL, config, cfg)

#define CURSOR_API_CALL(cur, s, n, bt)                                                     \
    (s) = CUR2S(cur);                                                                      \
    SESSION_API_PREPARE_CHECK(s, WT_CURSOR, n);                                            \
    API_CALL_NOCONF(s, WT_CURSOR, n, ((bt) == NULL) ? NULL : ((WT_BTREE *)(bt))->dhandle); \
    if (F_ISSET(cur, WT_CURSTD_CACHED))                                                    \
    WT_ERR(__wt_cursor_cached(cur))

#define CURSOR_API_CALL_PREPARE_ALLOWED(cur, s, n, bt)                                     \
    (s) = CUR2S(cur);                                                                      \
    API_CALL_NOCONF(s, WT_CURSOR, n, ((bt) == NULL) ? NULL : ((WT_BTREE *)(bt))->dhandle); \
    if (F_ISSET(cur, WT_CURSTD_CACHED))                                                    \
    WT_ERR(__wt_cursor_cached(cur))

#define JOINABLE_CURSOR_CALL_CHECK(cur) \
    if (F_ISSET(cur, WT_CURSTD_JOINED)) \
    WT_ERR(__wt_curjoin_joined(cur))

#define JOINABLE_CURSOR_API_CALL(cur, s, n, bt) \
    CURSOR_API_CALL(cur, s, n, bt);             \
    JOINABLE_CURSOR_CALL_CHECK(cur)

#define JOINABLE_CURSOR_API_CALL_PREPARE_ALLOWED(cur, s, n, bt) \
    CURSOR_API_CALL_PREPARE_ALLOWED(cur, s, n, bt);             \
    JOINABLE_CURSOR_CALL_CHECK(cur)

#define CURSOR_REMOVE_API_CALL(cur, s, bt)           \
    (s) = CUR2S(cur);                                \
    SESSION_API_PREPARE_CHECK(s, WT_CURSOR, remove); \
    TXN_API_CALL_NOCONF(s, WT_CURSOR, remove, ((bt) == NULL) ? NULL : ((WT_BTREE *)(bt))->dhandle)

#define JOINABLE_CURSOR_REMOVE_API_CALL(cur, s, bt) \
    CURSOR_REMOVE_API_CALL(cur, s, bt);             \
    JOINABLE_CURSOR_CALL_CHECK(cur)

#define CURSOR_UPDATE_API_CALL_BTREE(cur, s, n)                                               \
    (s) = CUR2S(cur);                                                                         \
    SESSION_API_PREPARE_CHECK(s, WT_CURSOR, n);                                               \
    TXN_API_CALL_NOCONF(s, WT_CURSOR, n, ((WT_CURSOR_BTREE *)(cur))->dhandle);                \
    if (F_ISSET(S2C(s), WT_CONN_IN_MEMORY) && !F_ISSET(CUR2BT(cur), WT_BTREE_IGNORE_CACHE) && \
      __wt_cache_full(s))                                                                     \
        WT_ERR(WT_CACHE_FULL);

#define CURSOR_UPDATE_API_CALL(cur, s, n)       \
    (s) = CUR2S(cur);                           \
    SESSION_API_PREPARE_CHECK(s, WT_CURSOR, n); \
    TXN_API_CALL_NOCONF(s, WT_CURSOR, n, NULL)

#define JOINABLE_CURSOR_UPDATE_API_CALL(cur, s, n) \
    CURSOR_UPDATE_API_CALL(cur, s, n);             \
    JOINABLE_CURSOR_CALL_CHECK(cur)

#define CURSOR_UPDATE_API_END_RETRY(s, ret, retry) \
    if ((ret) == WT_PREPARE_CONFLICT)              \
        (ret) = WT_ROLLBACK;                       \
    TXN_API_END(s, ret, retry)

#define CURSOR_UPDATE_API_END(s, ret) CURSOR_UPDATE_API_END_RETRY(s, ret, true)
