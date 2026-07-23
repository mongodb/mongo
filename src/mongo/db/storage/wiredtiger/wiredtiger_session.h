// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_compiled_configuration.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstdint>
#include <list>
#include <string>
#include <string_view>

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
     * @param engineEpoch In which session epoch was this session instantiated.
     * @param rtsEpoch In which rollback to stable epoch was this session instantiated.
     * @param config configuration string used to open the session with.
     * @param isInternal Mark current session as internal if true.
     */
    WiredTigerSession(WiredTigerConnection* connection,
                      uint64_t engineEpoch,
                      uint64_t rtsEpoch,
                      const char* config,
                      bool isInternal);

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
        ON_BLOCK_EXIT([&] { _storageEngineTime += timer.elapsed(); });          \
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
#undef WRAPPED_WT_SESSION_METHOD

    /**
     * Perform a WiredTiger-level verification of a table.
     *
     * The config string specifies optional extra arguments made to the verify call, nullptr and
     * empty strings are both permitted. These extra arguments will override any arguments that
     * this wrapper adds internally.
     *
     * TODO SERVER-131939: Once we stop unconditionally skipping the extra HS key verification,
     * remove this custom wrapper and go back to using the macro.
     */
    int verify(const char* uri, const char* config);

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
    WT_CURSOR* getNewCursor(std::string_view uri, const char* config);

    /**
     * Wrapper for getNewCursor() without a config string.
     */
    WT_CURSOR* getNewCursor(std::string_view uri) {
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

    Microseconds getStorageEngineTime() const {
        return _storageEngineTime;
    }

    /**
     * Helper for WT_SESSION::get_last_error.
     */
    GetLastError getLastError();

    /**
     * Setter used for testing to allow tick source to be mocked.
     */
    [[MONGO_MOD_PRIVATE]] void setTickSource_forTest(TickSource* tickSource) {
        _tickSource = tickSource;
    }

    bool isInternalSession() const {
        return _isInternalSession;
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

    void _openCursor(WT_SESSION* session,
                     std::string_view uri,
                     const char* config,
                     WT_CURSOR** cursorOut);

    // Used internally by WiredTigerConnection to get the engine epoch
    uint64_t _getEngineEpoch() const {
        return _engineEpoch;
    }

    // Used internally by WiredTigerConnection to get the rollback to stable epoch.
    uint64_t _getRtsEpoch() const {
        return _rtsEpoch;
    }

    // Ensure the engine epoch is checked before the roll back to stable epoch as it takes
    // precedence.
    const uint64_t _engineEpoch;
    const uint64_t _rtsEpoch;

    bool _isInternalSession = true;

    WT_SESSION* _session;  // owned
    CursorCache _cursors;  // owned
    uint64_t _cursorGen;
    int _cursorsOut;

    WiredTigerConnection* const _connection;  // not owned

    Date_t _idleExpireTime;

    // Tracks the cumulative duration of WT_SESSION API calls.
    Microseconds _storageEngineTime;

    // A set that contains the undo config strings for any reconfigurations we might have performed
    // on a session during the lifetime of this recovery unit. We use these to reset the session to
    // its default configuration before returning it to the session cache.
    stdx::unordered_set<std::string> _undoConfigStrings;

    TickSource* _tickSource = globalSystemTickSource();
};

}  // namespace mongo
