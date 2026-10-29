// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PARENT_PRIVATE]] SetClusterParameterCoordinator
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

    std::string_view serializePhase(const Phase& phase) const override {
        return idl::serialize(phase);
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
