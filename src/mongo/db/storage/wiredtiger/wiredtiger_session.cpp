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

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection)
    : WiredTigerSession(connection, nullptr, nullptr) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     uint64_t epoch,
                                     const char* config)
    : _epoch(epoch),
      _session(connection->_openSession(this, nullptr, config)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     WT_EVENT_HANDLER* handler,
                                     const char* config)
    : _epoch(0),
      _session(connection->_openSession(this, handler, config)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     StatsCollectionPermit& permit)
    : _epoch(0),
      _session(connection->_openSession(this, permit, nullptr)),
      _cursorGen(0),
      _cursorsOut(0),
      _connection(connection),
      _idleExpireTime(Date_t::min()) {}

WiredTigerSession::WiredTigerSession(WiredTigerConnection* connection,
                                     WT_EVENT_HANDLER* handler,
                                     StatsCollectionPermit& permit)
    : _epoch(0),
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
}

namespace {
void _openCursor(WT_SESSION* session, StringData uri, const char* config, WT_CURSOR** cursorOut) {
    int ret = session->open_cursor(session, uri.data(), nullptr, config, cursorOut);
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
                  str::stream() << "Failed to open a WiredTiger cursor. Reason: " << status
                                << ", uri: " << uri << ", config: " << config);
    }

    LOGV2_FATAL_NOTRACE(50882,
                        "Failed to open WiredTiger cursor. This may be due to data corruption",
                        "uri"_attr = uri,
                        "config"_attr = config,
                        "error"_attr = status,
                        "message"_attr = kWTRepairMsg);
}
}  // namespace

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

WT_CURSOR* WiredTigerSession::getNewCursor(StringData uri, const char* config) {
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
    if (_connection->isShuttingDown() || _getEpoch() < _connection->_epoch.load()) {
        return;
    }

    invariant(_session);
    invariant(cursor);
    _cursorsOut--;

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
    if (_connection->isShuttingDown() || _getEpoch() < _connection->_epoch.load()) {
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
