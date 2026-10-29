// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Interface for acquiring and releasing catalog resources needed for stages that need catalog
 * information (i.e. DocumentSourceCursor and DocumentSourceInternalSearchIdLookUp).
 */
class [[MONGO_MOD_PUBLIC]] CatalogResourceHandle : public RefCountable {
public:
    ~CatalogResourceHandle() override = default;

    virtual void acquire(OperationContext* opCtx) = 0;
    virtual void release() = 0;
    virtual void checkCanServeReads(OperationContext* opCtx, const PlanExecutor& exec) {
        // Default no-op.
    }
    virtual boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> getStasher()
        const = 0;
};

class [[MONGO_MOD_PRIVATE]] DSCatalogResourceHandleBase : public CatalogResourceHandle {
public:
    DSCatalogResourceHandleBase(
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher)
        : _transactionResourcesStasher(std::move(stasher)) {
        tassert(10096101,
                "Expected _transactionResourcesStasher to exist",
                _transactionResourcesStasher);
    }

    void acquire(OperationContext* opCtx) final;
    void release() final;

    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> getStasher() const final {
        return _transactionResourcesStasher;
    }

protected:
    bool isAcquired() const {
        return _resources.has_value();
    }

private:
    boost::optional<HandleTransactionResourcesFromStasher> _resources;
    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline>
        _transactionResourcesStasher;
    // Tracks the opCtx of the current command for non-ticketed aggregation interval timing.
    OperationContext* _lastOpCtx{nullptr};
};
}  // namespace mongo
