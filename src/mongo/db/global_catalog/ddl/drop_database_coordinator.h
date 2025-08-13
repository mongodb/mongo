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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/drop_database_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class DropDatabaseCoordinator final
    : public RecoverableShardingDDLCoordinator<DropDatabaseCoordinatorDocument,
                                               DropDatabaseCoordinatorPhaseEnum> {

public:
    using StateDoc = DropDatabaseCoordinatorDocument;
    using Phase = DropDatabaseCoordinatorPhaseEnum;

    DropDatabaseCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "DropDatabaseCoordinator", initialState),
          _dbName(nss().dbName()),
          _critSecReason(BSON("dropDatabase" << DatabaseNameUtil::serialize(
                                  _dbName, SerializationContext::stateCommandRequest()))) {}
    ~DropDatabaseCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const final {}

private:
    StringData serializePhase(const Phase& phase) const override {
        return DropDatabaseCoordinatorPhase_serializer(phase);
    }

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() > Phase::kUnset;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _dropTrackedCollection(OperationContext* opCtx,
                                const CollectionType& coll,
                                const ShardId& changeStreamsNotifierShardId,
                                std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                const CancellationToken& token);

    void _clearDatabaseInfoOnPrimary(OperationContext* opCtx);

    void _clearDatabaseInfoOnSecondaries(OperationContext* opCtx);

    DatabaseName _dbName;

    const BSONObj _critSecReason;
};

}  // namespace mongo
