// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/util/modules.h"

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
