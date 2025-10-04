/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class SetClusterParameterCoordinator
    : public ConfigsvrCoordinatorImpl<SetClusterParameterCoordinatorDocument,
                                      SetClusterParameterCoordinatorPhaseEnum> {
public:
    using StateDoc = SetClusterParameterCoordinatorDocument;
    using Phase = SetClusterParameterCoordinatorPhaseEnum;

    explicit SetClusterParameterCoordinator(const BSONObj& stateDoc)
        : ConfigsvrCoordinatorImpl(stateDoc) {}

    bool hasSameOptions(const BSONObj& participantDoc) const override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    /**
     * Returns 'true' if the coordinator instance has detected an unexpected concurrent update
     * operation.
     */
    bool detectedConcurrentUpdate() const {
        return _detectedConcurrentUpdate;
    }

private:
    friend class SetClusterParameterCoordinatorTest;
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    /*
     * Performs a local write with majority write concern to set the parameter.
     */
    void _commit(OperationContext* opCtx);

    /*
     * Returns the persisted cluster parameter value. Returns boost::none if the parameter has not
     * been set.
     */
    boost::optional<BSONObj> _getPersistedClusterParameter(OperationContext* opCtx) const;

    /*
     * Sends setClusterParameter to every shard in the cluster with the appropiate session.
     */
    void _sendSetClusterParameterToAllShards(
        OperationContext* opCtx,
        const OperationSessionInfo& opInfo,
        std::shared_ptr<executor::ScopedTaskExecutor> executor);

    const ConfigsvrCoordinatorMetadata& metadata() const override;

    StringData serializePhase(const Phase& phase) const override {
        return SetClusterParameterCoordinatorPhase_serializer(phase);
    }

    /*
     * Returns true if 'previousTime' does not match the value of "clusterParameterTime" field of
     * the cluster-wide parameter document 'currentClusterParameterValue'.
     */
    static bool _isPersistedStateConflictingWithPreviousTime(
        const boost::optional<LogicalTime>& previousTime,
        const boost::optional<BSONObj>& currentClusterParameterValue);

    /* Returns true if the cluster-wide parameter value given as variable 'parameter' is equal to
     * the cluster-wide parameter value given as the persisted parameter document
     * 'persistedParameter'.
     */
    static bool _parameterValuesEqual(const BSONObj& parameter, const BSONObj& persistedParameter);

    bool _detectedConcurrentUpdate = false;
};

}  // namespace mongo
