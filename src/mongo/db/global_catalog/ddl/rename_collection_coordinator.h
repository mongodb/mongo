/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_rename_collection_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class RenameCollectionCoordinator final
    : public RecoverableShardingDDLCoordinator<RenameCollectionCoordinatorDocument,
                                               RenameCollectionCoordinatorPhaseEnum> {
public:
    using StateDoc = RenameCollectionCoordinatorDocument;
    using Phase = RenameCollectionCoordinatorPhaseEnum;

    RenameCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& doc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    /**
     * Waits for the rename to complete and returns the collection version.
     */
    RenameCollectionResponse getResponse(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        tassert(10644505, "Expected _response to be set", _response);
        return *_response;
    }

protected:
    logv2::DynamicAttributes getCoordinatorLogAttrs() const override;

private:
    StringData serializePhase(const Phase& phase) const override {
        return RenameCollectionCoordinatorPhase_serializer(phase);
    }

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kFreezeMigrations;
    };

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx) override;

    boost::optional<RenameCollectionResponse> _response;
    const RenameCollectionRequest _request;
};

}  // namespace mongo
