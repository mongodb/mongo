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

#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Interface for acquiring and releasing catalog resources needed for stages that need catalog
 * information (i.e. DocumentSourceCursor and DocumentSourceInternalSearchIdLookUp).
 */
class MONGO_MOD_PUBLIC CatalogResourceHandle : public RefCountable {
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

class MONGO_MOD_PRIVATE DSCatalogResourceHandleBase : public CatalogResourceHandle {
public:
    DSCatalogResourceHandleBase(
        boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> stasher)
        : _transactionResourcesStasher(std::move(stasher)) {
        tassert(10096101,
                "Expected _transactionResourcesStasher to exist",
                _transactionResourcesStasher);
    }

    void acquire(OperationContext* opCtx) final {
        tassert(10271302, "Expected resources to be absent", !_resources);
        _resources.emplace(opCtx, _transactionResourcesStasher.get());
    }

    void release() final {
        _resources.reset();
    }

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
};
}  // namespace mongo
