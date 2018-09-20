/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/session_catalog.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

/**
 * This object handles acquiring the global exclusive lock for replication state transitions, as
 * well as any actions that need to happen in between enqueuing the global lock request and waiting
 * for it to be granted.  One of the main such actions is aborting all in-progress transactions and
 * causing all prepared transaction to yield their locks during the transition and restoring them
 * when the transition is complete.
 */
class ReplicationStateTransitionLockGuard {
    MONGO_DISALLOW_COPYING(ReplicationStateTransitionLockGuard);

public:
    struct Args {
        // How long to wait for the global X lock.
        Date_t lockDeadline = Date_t::max();

        // If true, will kill all user operations in between enqueuing the global lock request and
        // waiting for it to be granted.
        bool killUserOperations = false;
    };

    /**
     * Acquires the global X lock while yielding the locks held by any prepared transactions.
     * Also performs any other actions required according to the Args provided.
     */
    ReplicationStateTransitionLockGuard(OperationContext* opCtx, const Args& args);

    /**
     * Releases the global X lock and atomically restores the locks for prepared transactions that
     * were yielded in the constructor.
     */
    ~ReplicationStateTransitionLockGuard();

    /**
     * Temporarily releases the global X lock.  Must be followed by a call to
     * reacquireGlobalLockForStepdownAttempt().
     */
    void releaseGlobalLockForStepdownAttempt();

    /**
     * Requires the global X lock after it was released via a call to
     * releaseGlobalLockForStepdownAttempt().  Ignores the configured 'lockDeadline' and instead
     * waits forever for the lock to be acquired.
     */
    void reacquireGlobalLockForStepdownAttempt();

private:
    // OperationContext of the thread driving the state transition.
    OperationContext* const _opCtx;

    // Args to configure what behaviors need to be taken while acquiring the global X lock for the
    // state transition.
    Args _args;

    // The global X lock that this object is responsible for acquiring as part of the state
    // transition.
    boost::optional<Lock::GlobalLock> _globalLock;

    // Used to prevent Sessions from being checked out, so that we can wait for all sessions to be
    // checked in and iterate over all Sessions to get Sessions with prepared transactions to yield
    // their locks.
    boost::optional<SessionCatalog::PreventCheckingOutSessionsBlock> _preventCheckingOutSessions;

    // Locks that were held by prepared transactions and were yielded in order to allow taking the
    // global X lock.
    std::vector<std::pair<Locker*, Locker::LockSnapshot>> _yieldedLocks;
};

}  // namespace repl
}  // namespace mongo
