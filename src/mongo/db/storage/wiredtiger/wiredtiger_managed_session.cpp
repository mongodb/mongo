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
