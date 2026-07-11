// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/logv2/log.h"

#include <string_view>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection)
    : WiredTigerSession(connection, nullptr, nullptr) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     uint64_t engineEpoch,
                                     uint64_t rtsEpoch,
                                     const char* config,
                                     const bool isInternal)
    : _engineEpoch(engineEpoch),
      _rtsEpoch(rtsEpoch),
      _isInternalSession(isInternal),
      _session(connection->_openSession(this, nullptr, config)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     WT_EVENT_HANDLER* handler,
                                     const char* config)
    : _engineEpoch(0),
      _rtsEpoch(0),
      _session(connection->_openSession(this, handler, config)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     StatsCollectionPermit& permit)
    : _engineEpoch(0),
      _rtsEpoch(0),
      _session(connection->_openSession(this, permit, nullptr)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     WT_EVENT_HANDLER* handler,
                                     StatsCollectionPermit& permit)
    : _engineEpoch(0),
      _rtsEpoch(0),
      _session(connection->_openSession(this, handler, permit, nullptr)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::~WiredTigerSession() {
    detachOperationContext();
    if (_session) {
        invariantWTOK(_session->close(_session, nullptr), nullptr);
    }

    _connection->_closeSession(_isInternalSession);
}

void WiredTigerSession::_openCursor(WT_SESSION* session,
                                    std::string_view uri,
                                    const char* config,
                                    WT_CURSOR** cursorOut) {
    // TODO SERVER-128957: Dangerous assumption of null-terminated std::string_view.
    const char* uriData = uri.data();

    // TODO: SERVER-110391 Add an invariant here to catch stale sessions.
    int ret = session->open_cursor(session, uriData, nullptr, config, cursorOut);
    if (ret == 0) {
        return;
    }

    auto status = wtRCToStatus(ret, session);

    if (ret == EBUSY) {
        // This may happen when there is an ongoing full validation, with a call to WT::verify.
        // Other operations which may trigger this include salvage, rollback_to_stable, upgrade,
        // alter, or if there is a bulk cursor open. Mongo (currently) does not run any of
        // these operations concurrently with this code path, except for validation.

        uassertStatusOK(status);
    } else if (ret == ENOENT) {
        uasserted(ErrorCodes::CursorNotFound,
                  fmt::format("Failed to open a WiredTiger cursor. Reason: {}, uri: {}, config: {}",
                              status.toString(),
                              uri,
                              config ? std::string_view{config} : std::string_view{}));
    }

    LOGV2_FATAL_NOTRACE(50882,
                        "Failed to open WiredTiger cursor. This may be due to data corruption",
                        "uri"_attr = uri,
                        "config"_attr = config ? std::string_view{config} : std::string_view{},
                        "error"_attr = status,
                        "message"_attr = kWTRepairMsg);
}

WT_CURSOR* WiredTigerSession::getCachedCursor(uint64_t id, const std::string& config) {
    // Find the most recently used cursor
    for (CursorCache::iterator i = _cursors.begin(); i != _cursors.end(); ++i) {
        // Ensure that all properties of this cursor are identical to avoid mixing cursor
        // configurations. Note that this uses an exact string match, so cursor configurations with
        // parameters in different orders will not be considered equivalent.
        if (i->_id == id && i->_config == config) {
            WT_CURSOR* c = i->_cursor;
            _cursors.erase(i);
            _cursorsOut++;
            return c;
        }
    }
    return nullptr;
}

WT_CURSOR* WiredTigerSession::getNewCursor(std::string_view uri, const char* config) {
    WT_CURSOR* cursor = nullptr;
    _openCursor(_session, uri, config, &cursor);
    _cursorsOut++;
    return cursor;
}

void WiredTigerSession::releaseCursor(uint64_t id, WT_CURSOR* cursor, std::string config) {
    // When cleaning up a cursor, we would want to check if the connection is already in shutdown
    // and prevent the race condition that the shutdown starts after the check.
    WiredTigerConnection::BlockShutdown blockShutdown(_connection);

    // Avoids the cursor already being destroyed during the shutdown. Also, avoids releasing a
    // cursor from an earlier epoch.
    if (_connection->isShuttingDown() || _getEngineEpoch() < _connection->_engineEpoch.load()) {
        return;
    }

    invariant(_session);
    invariant(cursor);
    _cursorsOut--;

    // Do not put the cursor back in cache if the rollback to stable epoch has changed since we
    // acquired it. It is a stale cursor, so close it instead to ensure we do not leak cursors.
    // Always check the engine epoch first as that takes precedence.
    if (_getRtsEpoch() < _connection->_rtsEpoch.load()) {
        invariantWTOK(cursor->close(cursor), _session);
        return;
    }

    invariantWTOK(cursor->reset(cursor), _session);

    // Cursors are pushed to the front of the list and removed from the back
    _cursors.push_front(CachedCursor(id, _cursorGen++, cursor, std::move(config)));

    // A negative value for wiredTigercursorCacheSize means to use hybrid caching.
    std::uint32_t cacheSize = abs(gWiredTigerCursorCacheSize.load());

    while (!_cursors.empty() && _cursorGen - _cursors.back()._gen > cacheSize) {
        cursor = _cursors.back()._cursor;
        _cursors.pop_back();
        invariantWTOK(cursor->close(cursor), _session);
    }
}

void WiredTigerSession::closeCursor(WT_CURSOR* cursor) {
    // When cleaning up a cursor, we would want to check if the connection is already in shutdown
    // and prevent the race condition that the shutdown starts after the check.
    WiredTigerConnection::BlockShutdown blockShutdown(_connection);

    // Avoids the cursor already being destroyed during the shutdown. Also, avoids releasing a
    // cursor from an earlier epoch.
    if (_connection->isShuttingDown() || _getEngineEpoch() < _connection->_engineEpoch.load()) {
        return;
    }

    invariant(_session);
    invariant(cursor);
    _cursorsOut--;

    invariantWTOK(cursor->close(cursor), _session);
}

void WiredTigerSession::closeAllCursors(const std::string& uri) {
    invariant(_session);

    bool all = (uri == "");
    for (auto i = _cursors.begin(); i != _cursors.end();) {
        WT_CURSOR* cursor = i->_cursor;
        if (cursor && (all || uri == cursor->uri)) {
            invariantWTOK(cursor->close(cursor), _session);
            i = _cursors.erase(i);
        } else
            ++i;
    }
}

CompiledConfigurationsPerConnection* WiredTigerSession::getCompiledConfigurationsPerConnection() {
    return _connection->getCompiledConfigurations();
}

void WiredTigerSession::modifyConfiguration(const std::string& newConfig, std::string undoConfig) {
    if (newConfig == undoConfig) {
        // The undoConfig string is the config string that resets our session back to default
        // settings. If our new configuration is the same as the undoConfig string, then that means
        // that we are either setting our configuration back to default, or that the newConfig
        // string does not change our default values. In this case, we can erase the undoConfig
        // string from our set of undo config strings, since we no longer need to do any work to
        // restore the session to its default configuration.
        _undoConfigStrings.erase(undoConfig);
    } else {
        // Store the config string that will reset our session to its default configuration.
        _undoConfigStrings.emplace(std::move(undoConfig));
    }
    invariantWTOK(reconfigure(newConfig.c_str()), *this);
}

void WiredTigerSession::resetSessionConfiguration() {
    for (const std::string& undoConfigString : _undoConfigStrings) {
        invariantWTOK(reconfigure(undoConfigString.c_str()), *this);
    }
    _undoConfigStrings.clear();
}

void WiredTigerSession::attachOperationContext(OperationContext& opCtx) {
    invariant(_session);
    invariant(!_session->app_private);
    _session->app_private = &opCtx;
}

void WiredTigerSession::detachOperationContext() {
    if (_session) {
        _session->app_private = nullptr;
    }
}

WiredTigerSession::GetLastError WiredTigerSession::getLastError() {
    GetLastError getLastError;
    this->get_last_error(&getLastError.err, &getLastError.sub_level_err, &getLastError.err_msg);
    return getLastError;
}

}  // namespace mongo
