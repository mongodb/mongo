/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

namespace recoverable_critical_section_util {

bool inRecoveryMode(OperationContext* opCtx);

}

class ShardingRecoveryService : public ReplicaSetAwareServiceShardSvr<ShardingRecoveryService> {

public:
    ShardingRecoveryService() = default;

    static ShardingRecoveryService* get(ServiceContext* serviceContext);
    static ShardingRecoveryService* get(OperationContext* opCtx);

    /**
     * Acquires the collection critical section in the catch-up phase (i.e. blocking writes) for the
     * specified namespace and reason. It works even if the namespace's current metadata are
     * UNKNOWN.
     *
     * Entering into the Critical Section interrupts any ongoing filtering metadata refresh.
     *
     * It adds a doc to config.collectionCriticalSections with with writeConcern write concern.
     *
     * Do nothing if the collection critical section is taken for that nss and reason, and will
     * invariant otherwise since it is the responsibility of the caller to ensure that only one
     * thread is taking the critical section.
     */
    void acquireRecoverableCriticalSectionBlockWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const BSONObj& reason,
        const WriteConcernOptions& writeConcern,
        const boost::optional<BSONObj>& additionalInfo = boost::none);

    /**
     * Advances the recoverable critical section from the catch-up phase (i.e. blocking writes) to
     * the commit phase (i.e. blocking reads) for the specified nss and reason. The recoverable
     * critical section must have been acquired first through
     * 'acquireRecoverableCriticalSectionBlockWrites' function.
     *
     * It updates a doc from config.collectionCriticalSections with writeConcern write concern.
     *
     * Do nothing if the collection critical section is already taken in commit phase.
     */
    void promoteRecoverableCriticalSectionToBlockAlsoReads(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const BSONObj& reason,
                                                           const WriteConcernOptions& writeConcern);
    /**
     * Releases the recoverable critical section for the given nss and reason.
     *
     * It removes a doc from config.collectionCriticalSections with writeConcern write concern. As
     * part of the removal, the filtering information is cleared on secondary nodes. It is
     * responsability of the caller to properly set the filtering information on the primary node.
     *
     * Do nothing if the collection critical section is not taken for that nss and reason.
     */
    void releaseRecoverableCriticalSection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& reason,
                                           const WriteConcernOptions& writeConcern);

    /**
     * Recover all sharding related in memory states from disk.
     */
    void recoverStates(OperationContext* opCtx,
                       const std::set<NamespaceString>& rollbackNamespaces);

private:
    /**
     * This method is called when we have to mirror the state on disk of the recoverable critical
     * section to memory (on startUp or on rollback).
     */
    void recoverRecoverableCriticalSections(OperationContext* opCtx);

    /**
     * Recover the index versions from disk into the CSR.
     */
    void recoverIndexesCatalog(OperationContext* opCtx);

    void onInitialDataAvailable(OperationContext* opCtx,
                                bool isMajorityDataAvailable) override final {
        recoverRecoverableCriticalSections(opCtx);
        recoverIndexesCatalog(opCtx);
    }

    void onStartup(OperationContext* opCtx) override final {}
    void onShutdown() override final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override final {}
    void onStepUpComplete(OperationContext* opCtx, long long term) override final {}
    void onStepDown() override final {}
    void onBecomeArbiter() override final {}
};

}  // namespace mongo
