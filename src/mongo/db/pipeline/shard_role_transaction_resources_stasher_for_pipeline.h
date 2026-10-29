// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Class that holds the ShardRole::TransactionResources associated with the ShardRole state for a
 * pipeline. Stages of a pipeline that access local collections will have shared references to an
 * object of this class. When such a stages wants to use their ShardRole CollectionAcquisitions
 * it first needs to unstash the TransactionResources held by this object. The stage must stash back
 * the TransactionResources before handing control over to the next stage in the pipeline.
 * Objects of this class are not thread-safe and therefore should not be shared across threads.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardRoleTransactionResourcesStasherForPipeline
    : public TransactionResourcesStasher,
      public RefCountable {
public:
    StashedTransactionResources releaseStashedTransactionResources() override {
        return std::move(_transactionResources);
    }
    void stashTransactionResources(StashedTransactionResources resources) override {
        _transactionResources = std::move(resources);
    }

    ~ShardRoleTransactionResourcesStasherForPipeline() override {
        _transactionResources.dispose();
    }

protected:
    StashedTransactionResources _transactionResources;
};

}  // namespace mongo
