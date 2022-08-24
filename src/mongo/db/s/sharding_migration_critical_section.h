/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/util/future.h"

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
class ShardingMigrationCriticalSection {
    ShardingMigrationCriticalSection(const ShardingMigrationCriticalSection&) = delete;
    ShardingMigrationCriticalSection& operator=(const ShardingMigrationCriticalSection&) = delete;

public:
    ShardingMigrationCriticalSection();
    ~ShardingMigrationCriticalSection();

    /**
     * Enters the critical section in a mode, which still allows reads.
     *
     * NOTE: Must be called under the appropriate X lock (collection or database).
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
