// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"


namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Used to detect when a shard's shardIdentity document has been rolled back.  Since rolling back
 * the shardIdentity document is illegal (as we can't clear the in-memory state associated with it),
 * we force the mongod to shut down if it happens.  We detect shardIdentity rollback by checking in
 * the OpObserver for that document to be deleted.  We can't shut down right at that moment,
 * however, as doing so would interrupt the rollback process and leave the document in place for
 * when the server is restarted.  Instead, when we detect the shardIdentity document being deleted,
 * we call recordThatRollbackHappened() on this class, which records the fact that the shardIdentity
 * document was deleted as part of the rollback, and then when we exit rollback we check
 * didRollbackHappen() and shut down if so.
 *
 * No concurrency control is needed in this class as recordThatRollbackHappened can only happen in
 * an OpObserver detecting that the document was deleted, and didRollbackHappen is only called on
 * exiting rollback, and there can only be one of those things happening at any given time.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] ShardIdentityRollbackNotifier {
    ShardIdentityRollbackNotifier(const ShardIdentityRollbackNotifier&) = delete;
    ShardIdentityRollbackNotifier& operator=(const ShardIdentityRollbackNotifier&) = delete;

public:
    ShardIdentityRollbackNotifier();

    /**
     * Retrieves the ShardIdentityRollbackNotifier associated with the specified service context.
     */
    static ShardIdentityRollbackNotifier* get(OperationContext* opCtx);
    static ShardIdentityRollbackNotifier* get(ServiceContext* opCtx);

    /**
     * Records the fact that the shardIdentity document was rolled back.
     */
    void recordThatRollbackHappened() {
        _rollbackHappened = true;
    }

    /**
     * Checks whether the shardIdentity document was rolled back.
     */
    bool didRollbackHappen() {
        return _rollbackHappened;
    }

private:
    // Whether or not a rollback of the shardIdentity document has been detected.
    // No concurrency control necessary since this is only consulted during
    bool _rollbackHappened = false;
};

}  // namespace mongo
