// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Implements the critical section logic for particular collection or database in the sharding
 * subsystem. It supports two phases - catch-up and commit. During the catch-up phase, only writes
 * are disallowed, but reads can still proceed. In the commit phase, both reads and writes are
 * disallowed.
 *
 * Currently, only collections stay in the catch-up phase while the last batch of mods is
 * transferred to the recipient shard. Databases effectively only support the commit phase.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingMigrationCriticalSection {
    ShardingMigrationCriticalSection(const ShardingMigrationCriticalSection&) = delete;
    ShardingMigrationCriticalSection& operator=(const ShardingMigrationCriticalSection&) = delete;

public:
    ShardingMigrationCriticalSection();
    ~ShardingMigrationCriticalSection();

    /**
     * Enters the critical section in a mode, which still allows reads.
     *
     * NOTE: Must be called under the appropriate X lock in order to block writers/locked readers
     * (collection or database).
     */
    void enterCriticalSectionCatchUpPhase(const BSONObj& reason);

    /**
     * Sets the critical section in a mode, which disallows reads.
     */
    void enterCriticalSectionCommitPhase(const BSONObj& reason);

    /**
     * Leaves the critical section.
     */
    void exitCriticalSection(const BSONObj& reason);

    /**
     * Leaves the critical section without doing error-checking. Only meant to be used when
     * recovering the critical sections in the ShardingRecoveryService.
     */
    void exitCriticalSectionNoChecks();

    /**
     * Sets the critical section back to the catch up phase, which disallows reads.
     */
    void rollbackCriticalSectionCommitPhaseToCatchUpPhase(const BSONObj& reason);

    /**
     * Retrieves a critical section future to wait on. Will return boost::none if the migration is
     * not yet in the critical section or if the caller is a reader and the migration is not yet in
     * the commit phase.
     */
    enum Operation { kRead, kWrite };
    boost::optional<SharedSemiFuture<void>> getSignal(Operation op) const;

    boost::optional<BSONObj> getReason() const;

private:
    struct CriticalSectionContext {
        CriticalSectionContext(BSONObj reason_) : reason(std::move(reason_)) {}
        // Whether the migration source is in a critical section. Tracked as a shared promise so
        // that callers don't have to hold metadata locks in order to wait on it.
        SharedPromise<void> critSecSignal;

        // Used to delay blocking reads up until the commit of the metadata on the config server
        // needs to happen. This allows the shard to serve reads up until the config server metadata
        // update needs to be committed.
        //
        // The transition from false to true is protected by the database or collection X-lock,
        // which happens just before the config server metadata commit is scheduled.
        bool readsShouldWaitOnCritSec{false};

        // Information about the operation that originally acquired the critical section. Used to
        // make the operations that modify the state of the CS idempotent and to provide more
        // detailed error messages.
        BSONObj reason;
    };

    boost::optional<CriticalSectionContext> _critSecCtx;
};

}  // namespace mongo
