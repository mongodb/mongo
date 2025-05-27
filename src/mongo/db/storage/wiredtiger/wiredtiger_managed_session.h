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

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"

#include <memory>

namespace mongo {

/**
 * This class wraps a WiredTigerSession and manage the release of the Session to the Session
 * Cache. Copying is not allowed (move only).
 */
class WiredTigerManagedSession {
public:
    explicit WiredTigerManagedSession(std::unique_ptr<WiredTigerSession> session = nullptr);

    ~WiredTigerManagedSession();

    // Allow move construction
    WiredTigerManagedSession(WiredTigerManagedSession&& other) noexcept;
    WiredTigerManagedSession& operator=(WiredTigerManagedSession&& other) noexcept;

    // Disable copy construction and copy assignment
    WiredTigerManagedSession(const WiredTigerManagedSession&) = delete;
    WiredTigerManagedSession& operator=(const WiredTigerManagedSession&) = delete;

    /**
     * Overloaded -> operator to allow direct access to WiredTigerSession.
     * This mimics std::unique_ptr behavior. This class retains the ownership of the returned
     * pointer.
     */
    WiredTigerSession* operator->() const;

    /**
     * Overloaded * operator to allow dereferencing to the WiredTigerSession.
     * This mimics std::unique_ptr behavior.
     */
    WiredTigerSession& operator*() const;

    /**
     * Return direct access to WiredTigerSession.
     * This mimics std::unique_ptr behavior. This class retains the ownership of the returned
     * pointer.
     */
    WiredTigerSession* get() const;

    /**
     * Overloaded bool() operator to check if the enclosed session has not been released.
     * This mimics std::unique_ptr behavior.
     */
    explicit operator bool() const;


private:
    /**
     * Release the held session.
     */
    void _release();

    std::unique_ptr<WiredTigerSession> _session;
};
}  // namespace mongo
