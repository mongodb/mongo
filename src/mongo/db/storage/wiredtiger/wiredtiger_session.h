/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <list>
#include <string>

#include <wiredtiger.h>

namespace mongo {

class StatsCollectionPermit;
class WiredTigerConnection;
class OperationContext;

/**
 * This is a structure that caches 1 cursor for each uri.
 * The idea is that there is a pool of these somewhere.
 * NOT THREADSAFE
 */
class WiredTigerSession {
public:
    struct GetLastError {
        int err = 0;
        int sub_level_err = WT_NONE;
        const char* err_msg = "";
    };

    /**
     * Creates a new WT session on the specified connection. This session will not be cached.
     *
     * @param connection Wrapped WT connection
     */
    WiredTigerSession(WiredTigerConnection* connection);

    /**
     * Creates a new WT session on the specified connection. This is for sessions which will be
     * cached by the WiredTigerConnection.
     *
     * @param connection Wrapped WT connection
     * @param epoch In which session cache cleanup epoch was this session instantiated.
     */
    WiredTigerSession(WiredTigerConnection* connection, uint64_t epoch, const char* config);

    /**
     * Creates a new WT session on the specified connection. This session will not be cached.
     *
     * @param connection WT connection
     * @param handler Callback handler that will be invoked by wiredtiger.
     * @param config configuration string used to open the session with.
     */
    WiredTigerSession(WiredTigerConnection* connection,
                      WT_EVENT_HANDLER* handler,
                      const char* config);

    /**
     * Creates a new WT session for collecting statistics possibly during shutdown (but before
     * wiredtiger itself shuts down).
     *
     * @param connection WT connection
     * @param permit Token showing that you asked the engine for permission to open this session.
     */
    WiredTigerSession(WiredTigerConnection* connection, StatsCollectionPermit& permit);

    /**
     * Creates a new WT session for collecting statistics possibly during shutdown (but before
     * wiredtiger itself shuts down).
     *
     * @param handler Callback handler that will be invoked by wiredtiger.
     * @param connection WT connection
     * @param permit Token showing that you asked the engine for permission to open this session.
     */
    WiredTigerSession(WiredTigerConnection* connection,
                      WT_EVENT_HANDLER* handler,
                      StatsCollectionPermit& permit);

    ~WiredTigerSession();

    WiredTigerConnection& getConnection() {
        return *_connection;
    }

    // Safe accessor for the internal session
    template <typename Functor>
    auto with(Functor functor) {
        return functor(_session);
    }

#define WRAPPED_WT_SESSION_METHOD(name)                                         \
    decltype(auto) name(auto&&... args) {                                       \
        Timer timer(_tickSource);                                               \
        ON_BLOCK_EXIT([&] { _storageExecutionTime += timer.elapsed(); });       \
        return _session->name(_session, std::forward<decltype(args)>(args)...); \
    }

    WRAPPED_WT_SESSION_METHOD(alter)
    WRAPPED_WT_SESSION_METHOD(begin_transaction)
    WRAPPED_WT_SESSION_METHOD(checkpoint)
    WRAPPED_WT_SESSION_METHOD(commit_transaction)
    WRAPPED_WT_SESSION_METHOD(compact)
    WRAPPED_WT_SESSION_METHOD(create)
    WRAPPED_WT_SESSION_METHOD(drop)
    WRAPPED_WT_SESSION_METHOD(get_last_error)
    WRAPPED_WT_SESSION_METHOD(log_flush)
    WRAPPED_WT_SESSION_METHOD(open_cursor)
    WRAPPED_WT_SESSION_METHOD(prepare_transaction)
    WRAPPED_WT_SESSION_METHOD(query_timestamp)
    WRAPPED_WT_SESSION_METHOD(reset)
    WRAPPED_WT_SESSION_METHOD(reconfigure)
    WRAPPED_WT_SESSION_METHOD(rollback_transaction)
    WRAPPED_WT_SESSION_METHOD(salvage)
    WRAPPED_WT_SESSION_METHOD(timestamp_transaction_uint)
    WRAPPED_WT_SESSION_METHOD(transaction_pinned_range)
    WRAPPED_WT_SESSION_METHOD(truncate)
    WRAPPED_WT_SESSION_METHOD(verify)
#undef WRAPPED_WT_SESSION_METHOD

    /**
     * Gets a cursor on the table id 'id' with optional configuration, 'config'.
     *
     * This may return a cursor from the cursor cache and these cursors should *always* be released
     * into the cache by calling releaseCursor().
     */
    WT_CURSOR* getCachedCursor(uint64_t id, const std::string& config);

    /**
     * Create a new cursor and ignore the cache.
     *
     * The config string specifies optional arguments for the cursor. For example, when
     * the config contains 'read_once=true', this is intended for operations that will be
     * sequentially scanning large amounts of data.
     *
     * This will never return a cursor from the cursor cache, and these cursors should *never* be
     * released into the cache by calling releaseCursor(). Use closeCursor() instead.
     */
    WT_CURSOR* getNewCursor(StringData uri, const char* config);

    /**
     * Wrapper for getNewCursor() without a config string.
     */
    WT_CURSOR* getNewCursor(StringData uri) {
        return getNewCursor(uri, nullptr);
    }

    /**
     * Release a cursor into the cursor cache and close old cursors if the number of cursors in the
     * cache exceeds wiredTigerCursorCacheSize.
     * The exact cursor config that was used to create the cursor must be provided or subsequent
     * users will retrieve cursors with incorrect configurations.
     *
     * Additionally calls into the WiredTigerKVEngine to see if the SizeStorer needs to be flushed.
     * The SizeStorer gets flushed on a periodic basis.
     */
    void releaseCursor(uint64_t id, WT_CURSOR* cursor, std::string config);

    /**
     * Close a cursor without releasing it into the cursor cache.
     */
    void closeCursor(WT_CURSOR* cursor);

    /**
     * Closes all cached cursors matching the uri.  If the uri is empty,
     * all cached cursors are closed.
     */
    void closeAllCursors(const std::string& uri);

    int cursorsOut() const {
        return _cursorsOut;
    }

    int cachedCursors() const {
        return _cursors.size();
    }

    void setIdleExpireTime(Date_t idleExpireTime) {
        _idleExpireTime = idleExpireTime;
    }

    Date_t getIdleExpireTime() const {
        return _idleExpireTime;
    }

    CompiledConfigurationsPerConnection* getCompiledConfigurationsPerConnection();

    /**
     * Reconfigures the session. Stores the config string that undoes this change when
     * resetSessionConfiguration() is called.
     */
    void modifyConfiguration(const std::string& newConfig, std::string undoConfig);

    /**
     * Reset the configurations for this session to the default. This should be done before we
     * release this session back into the session cache, so that any recovery unit that may use this
     * session in the future knows that the session will have the default configuration.
     */
    void resetSessionConfiguration();

    stdx::unordered_set<std::string> getUndoConfigStrings() {
        return _undoConfigStrings;
    }

    /**
     * Drops this session immediately (without calling close()). This may be necessary during
     * shutdown to avoid racing against the connection's close. Only call this method if you're
     * about to delete the session.
     */
    void dropSessionBeforeDeleting() {
        invariant(_session);
        _session = nullptr;
    }

    /**
     * Attach an recovery that acts as an interrupt source and contains other relevant
     * state. WT will periodically use callbacks to check whether specific WT operations should be
     * interrupted.
     */
    void attachOperationContext(OperationContext& opCtx);

    /**
     * Remove the recovery unit.
     */
    void detachOperationContext();

    Microseconds getStorageExecutionTime() const {
        return _storageExecutionTime;
    }

    /**
     * Helper for WT_SESSION::get_last_error.
     */
    GetLastError getLastError();

    /**
     * Setter used for testing to allow tick source to be mocked.
     */
    void setTickSource_forTest(TickSource* tickSource) {
        _tickSource = tickSource;
    }

private:
    class CachedCursor {
    public:
        CachedCursor(uint64_t id, uint64_t gen, WT_CURSOR* cursor, std::string config)
            : _id(id), _gen(gen), _cursor(cursor), _config(std::move(config)) {}

        uint64_t _id;   // Source ID, assigned to each URI
        uint64_t _gen;  // Generation, used to age out old cursors
        WT_CURSOR* _cursor;
        std::string _config;  // Cursor config. Do not serve cursors with different configurations
    };

    friend class WiredTigerConnection;
    friend class WiredTigerManagedSession;

    // The cursor cache is a list of pairs that contain an ID and cursor
    typedef std::list<CachedCursor> CursorCache;

    // Used internally by WiredTigerConnection
    uint64_t _getEpoch() const {
        return _epoch;
    }

    const uint64_t _epoch;

    WT_SESSION* _session;  // owned
    CursorCache _cursors;  // owned
    uint64_t _cursorGen;
    int _cursorsOut;

    WiredTigerConnection* const _connection;  // not owned

    Date_t _idleExpireTime;

    // Tracks the cumulative duration of WT_SESSION API calls.
    Microseconds _storageExecutionTime;

    // A set that contains the undo config strings for any reconfigurations we might have performed
    // on a session during the lifetime of this recovery unit. We use these to reset the session to
    // its default configuration before returning it to the session cache.
    stdx::unordered_set<std::string> _undoConfigStrings;

    TickSource* _tickSource = globalSystemTickSource();
};

}  // namespace mongo
