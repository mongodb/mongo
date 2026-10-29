// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_managed_session.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/util/assert_util.h"

namespace mongo {

WiredTigerManagedSession::WiredTigerManagedSession(std::unique_ptr<WiredTigerSession> session)
    : _session(std::move(session)) {}

WiredTigerManagedSession::~WiredTigerManagedSession() {
    _release();
}

void WiredTigerManagedSession::_release() {
    if (_session) {
        _session->_connection->_releaseSession(std::move(_session));
    }
}

WiredTigerManagedSession::WiredTigerManagedSession(WiredTigerManagedSession&& other) noexcept
    : _session(std::move(other._session)) {}

WiredTigerManagedSession& WiredTigerManagedSession::operator=(
    WiredTigerManagedSession&& other) noexcept {
    if (this != &other) {
        _release();
        _session = std::move(other._session);
    }
    return *this;
}

WiredTigerSession* WiredTigerManagedSession::operator->() const {
    return get();
}

WiredTigerSession* WiredTigerManagedSession::get() const {
    invariant(_session);
    return _session.get();
}

WiredTigerSession& WiredTigerManagedSession::operator*() const {
    invariant(_session);
    return *_session;
}

WiredTigerManagedSession::operator bool() const {
    return _session != nullptr;
}
}  // namespace mongo
