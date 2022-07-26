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


#include "mongo/platform/basic.h"

#include "mongo/db/s/config/set_cluster_parameter_coordinator.h"

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};
}

bool SetClusterParameterCoordinator::hasSameOptions(const BSONObj& otherDocBSON) const {
    const auto otherDoc =
        StateDoc::parse(IDLParserContext("SetClusterParameterCoordinatorDocument"), otherDocBSON);
    return SimpleBSONObjComparator::kInstance.evaluate(_doc.getParameter() ==
                                                       otherDoc.getParameter());
}

boost::optional<BSONObj> SetClusterParameterCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    BSONObjBuilder cmdBob;
    cmdBob.appendElements(_doc.getParameter());

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "SetClusterParameterCoordinator");
    bob.append("op", "command");
    bob.append("currentPhase", _doc.getPhase());
    bob.append("command", cmdBob.obj());
    bob.append("active", true);
    return bob.obj();
}

void SetClusterParameterCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(6343101,
                2,
                "SetClusterParameterCoordinator phase transition",
                "newPhase"_attr = SetClusterParameterCoordinatorPhase_serializer(newDoc.getPhase()),
                "oldPhase"_attr = SetClusterParameterCoordinatorPhase_serializer(_doc.getPhase()));

    auto opCtx = cc().makeOperationContext();

    if (_doc.getPhase() == Phase::kUnset) {
        PersistentTaskStore<StateDoc> store(NamespaceString::kConfigsvrCoordinatorsNamespace);
        try {
            store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // A series of step-up and step-down events can cause a node to try and insert the
            // document when it has already been persisted locally, but we must still wait for
            // majority commit.
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
            const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
            WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(lastLocalOpTime, opCtx.get()->getCancellationToken())
                .get(opCtx.get());
        }
    } else {
        _updateStateDocument(opCtx.get(), newDoc);
    }

    _doc = std::move(newDoc);
}

bool SetClusterParameterCoordinator::_isClusterParameterSetAtTimestamp(OperationContext* opCtx) {
    auto parameterElem = _doc.getParameter().firstElement();
    auto parameterName = parameterElem.fieldName();
    auto parameter = _doc.getParameter()[parameterName].Obj();
    auto configsvrParameters =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            repl::ReadConcernLevel::kMajorityReadConcern,
            NamespaceString::kClusterParametersNamespace,
            BSON("_id" << parameterName << "clusterParameterTime"
                       << *_doc.getClusterParameterTime()),
            BSONObj(),
            boost::none));

    dassert(configsvrParameters.docs.size() <= 1);

    return !configsvrParameters.docs.empty();
}

void SetClusterParameterCoordinator::_sendSetClusterParameterToAllShards(
    OperationContext* opCtx,
    const OperationSessionInfo& session,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    auto shards = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    LOGV2_DEBUG(6387001, 1, "Sending setClusterParameter to shards:", "shards"_attr = shards);

    ShardsvrSetClusterParameter request(_doc.getParameter());
    request.setDbName(NamespaceString::kAdminDb);
    request.setClusterParameterTime(*_doc.getClusterParameterTime());
    sharding_util::sendCommandToShards(
        opCtx,
        NamespaceString::kAdminDb,
        CommandHelpers::appendMajorityWriteConcern(request.toBSON(session.toBSON())),
        shards,
        **executor);
}

void SetClusterParameterCoordinator::_commit(OperationContext* opCtx) {
    LOGV2_DEBUG(6387002, 1, "Updating configsvr cluster parameter");

    SetClusterParameter setClusterParameterRequest(_doc.getParameter());
    setClusterParameterRequest.setDbName(NamespaceString::kAdminDb);
    std::unique_ptr<ServerParameterService> parameterService =
        std::make_unique<ClusterParameterService>();
    DBDirectClient client(opCtx);
    ClusterParameterDBClientService dbService(client);
    SetClusterParameterInvocation invocation{std::move(parameterService), dbService};
    invocation.invoke(
        opCtx, setClusterParameterRequest, _doc.getClusterParameterTime(), kMajorityWriteConcern);
}

const ConfigsvrCoordinatorMetadata& SetClusterParameterCoordinator::metadata() const {
    return _doc.getConfigsvrCoordinatorMetadata();
}

ExecutorFuture<void> SetClusterParameterCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // Select a cluster parameter time only once, when the coordinator is run the first
            // time, this way, even if the process steps down while sending the command to the
            // shards, on the next run will use the same time for the remaining shards.
            if (!_doc.getClusterParameterTime()) {
                // Select a clusterParameter time.
                auto vt = VectorClock::get(opCtx)->getTime();
                auto clusterParameterTime = vt.clusterTime();
                _doc.setClusterParameterTime(clusterParameterTime.asTimestamp());
            }
        })
        .then(_executePhase(
            Phase::kSetClusterParameter, [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "setClusterParameter.start",
                    NamespaceString::kClusterParametersNamespace.toString(),
                    _doc.getParameter(),
                    kMajorityWriteConcern);

                // If the parameter was already set on the config server, there is
                // nothing else to do.
                if (_isClusterParameterSetAtTimestamp(opCtx)) {
                    return;
                }

                _doc = _updateSession(opCtx, _doc);
                const auto session = _getCurrentSession();

                {
                    // Ensure the topology is stable so shards added concurrently will
                    // not miss the cluster parameter. Keep it stable until we have
                    // persisted the cluster parameter on the configsvr so that new
                    // shards that get added will see the new cluster parameter.
                    Lock::SharedLock stableTopologyRegion =
                        ShardingCatalogManager::get(opCtx)->enterStableTopologyRegion(opCtx);

                    _sendSetClusterParameterToAllShards(opCtx, session, executor);

                    _commit(opCtx);
                }

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "setClusterParameter.end",
                    NamespaceString::kClusterParametersNamespace.toString(),
                    _doc.getParameter(),
                    kMajorityWriteConcern);
            }));
}

}  // namespace mongo
