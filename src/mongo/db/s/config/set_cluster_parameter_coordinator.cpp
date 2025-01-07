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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/cluster_server_parameter_cmds_gen.h"
#include "mongo/db/commands/set_cluster_parameter_invocation.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/config/set_cluster_parameter_coordinator.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/future_impl.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

/*
 * Returns the "clusterParameterTime" attribute value in 'parameterDoc'.
 */
Timestamp parseClusterParameterTime(const BSONObj& parameterDoc) {
    BSONElement clusterParameterTimeElem = parameterDoc.getField(
        SetClusterParameterCoordinatorDocument::kClusterParameterTimeFieldName);
    dassert(!clusterParameterTimeElem.eoo() && !clusterParameterTimeElem.isNull());
    return clusterParameterTimeElem.timestamp();
}

const int kNumberOfSystemFieldsInParameterDocument = 2;
}  // namespace

bool SetClusterParameterCoordinator::hasSameOptions(const BSONObj& otherDocBSON) const {
    const auto otherDoc =
        StateDoc::parse(IDLParserContext("SetClusterParameterCoordinatorDocument"), otherDocBSON);

    return _evalStateDocumentThreadSafe([&](const StateDoc& doc) -> bool {
        return SimpleBSONObjComparator::kInstance.evaluate(doc.getParameter() ==
                                                           otherDoc.getParameter()) &&
            doc.getTenantId() == otherDoc.getTenantId();
    });
}

boost::optional<BSONObj> SetClusterParameterCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder bob;

    _evalStateDocumentThreadSafe([&](const StateDoc& doc) {
        BSONObjBuilder cmdBob;
        cmdBob.appendElements(_doc.getParameter());

        bob.append("type", "op");
        bob.append("desc", "SetClusterParameterCoordinator");
        bob.append("op", "command");
        auto tenantId = _doc.getTenantId();
        if (tenantId.is_initialized()) {
            bob.append("tenantId", tenantId->toString());
        }
        bob.append("currentPhase", _doc.getPhase());
        bob.append("command", cmdBob.obj());
        bob.append("active", true);
    });

    return bob.obj();
}

bool SetClusterParameterCoordinator::_parameterValuesEqual(const BSONObj& parameter,
                                                           const BSONObj& persistedParameter) {
    // Check if the number of fields in 'parameter' match the number of fields in the persisted
    // cluster-wide parameter document while ignoring "_id" and "clusterParameterTime" fields.
    if (persistedParameter.nFields() !=
        parameter.nFields() + kNumberOfSystemFieldsInParameterDocument) {
        return false;
    }
    for (auto&& element : parameter) {
        if (!element.binaryEqualValues(
                persistedParameter.getField(element.fieldNameStringData()))) {
            return false;
        }
    }
    return true;
}

bool SetClusterParameterCoordinator::_isPersistedStateConflictingWithPreviousTime(
    const boost::optional<LogicalTime>& previousTime,
    const boost::optional<BSONObj>& currentClusterParameterValue) {
    // If optimistic locking is not used, there is no update conflict.
    if (!previousTime) {
        return false;
    }

    // "previousTime" is provided, thus check whether the cluster parameter value was modified (by a
    // concurrent update) since 'previousTime' by comparing "clusterParameterTime" stored on disk
    // with 'previousTime'. The 'previousTime' equal to 'LogicalTime::kUninitialized' denotes a
    // special case when the cluster parameter value is still unset. In such a case, we expect that
    // 'currentClusterParameterValue' does not have a value.
    if (*previousTime == LogicalTime::kUninitialized) {
        return currentClusterParameterValue.has_value();
    }
    if (!currentClusterParameterValue) {
        return true;
    }
    return parseClusterParameterTime(*currentClusterParameterValue) != previousTime->asTimestamp();
}

boost::optional<BSONObj> SetClusterParameterCoordinator::_getPersistedClusterParameter(
    OperationContext* opCtx) const {
    auto parameterName = _doc.getParameter().firstElement().fieldName();
    const auto& configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();
    auto configsvrParameters = uassertStatusOK(configShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString::makeClusterParametersNSS(_doc.getTenantId()),
        BSON("_id" << parameterName),
        BSONObj(),
        boost::none));

    dassert(configsvrParameters.docs.size() <= 1);

    if (configsvrParameters.docs.empty()) {
        return boost::none;
    }

    BSONObj& parameterDoc = configsvrParameters.docs.front();
    LOGV2_DEBUG(9469100,
                1,
                "loaded cluster parameter",
                "coordinatorState"_attr = _doc.toBSON(),
                "document"_attr = parameterDoc,
                "parameterName"_attr = parameterName);
    return boost::optional<BSONObj>(parameterDoc.getOwned());
}

void SetClusterParameterCoordinator::_sendSetClusterParameterToAllShards(
    OperationContext* opCtx,
    const OperationSessionInfo& session,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    auto shards = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    LOGV2_DEBUG(6387001, 1, "Sending setClusterParameter to shards:", "shards"_attr = shards);

    ShardsvrSetClusterParameter request(_doc.getParameter());
    request.setDbName(DatabaseNameUtil::deserialize(_doc.getTenantId(),
                                                    DatabaseName::kAdmin.db(omitTenant),
                                                    SerializationContext::stateDefault()));
    request.setClusterParameterTime(*_doc.getClusterParameterTime());
    sharding_util::sendCommandToShards(
        opCtx,
        DatabaseName::kAdmin,
        CommandHelpers::appendMajorityWriteConcern(request.toBSON(session.toBSON())),
        shards,
        **executor);
}

void SetClusterParameterCoordinator::_commit(OperationContext* opCtx) {
    LOGV2_DEBUG(6387002, 1, "Updating configsvr cluster parameter");

    SetClusterParameter setClusterParameterRequest(_doc.getParameter());
    setClusterParameterRequest.setDbName(
        DatabaseNameUtil::deserialize(_doc.getTenantId(),
                                      DatabaseName::kAdmin.db(omitTenant),
                                      SerializationContext::stateDefault()));
    std::unique_ptr<ServerParameterService> parameterService =
        std::make_unique<ClusterParameterService>();
    DBDirectClient client(opCtx);
    ClusterParameterDBClientService dbService(client);
    SetClusterParameterInvocation invocation{std::move(parameterService), dbService};
    invocation.invoke(opCtx,
                      setClusterParameterRequest,
                      _doc.getClusterParameterTime(),
                      boost::none /* previousTime */,
                      kMajorityWriteConcern,
                      true /* skipValidation */);
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

                _updateStateDocumentWith(opCtx, [&](StateDoc& doc) {
                    doc.setClusterParameterTime(clusterParameterTime.asTimestamp());
                });
            }
        })
        .then(_buildPhaseHandler(
            Phase::kSetClusterParameter, [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                // Get cluster parameter value stored on disk.
                const auto persistedClusterParameter = _getPersistedClusterParameter(opCtx);

                // If the parameter is already set on the config server, do a no-op.
                if (persistedClusterParameter) {
                    bool isClusterTimeEqual =
                        parseClusterParameterTime(*persistedClusterParameter) ==
                        *_doc.getClusterParameterTime();
                    if (isClusterTimeEqual)
                        return;

                    bool isParameterValueEqual = _parameterValuesEqual(
                        _doc.getParameter().firstElement().Obj(), *persistedClusterParameter);
                    if (isParameterValueEqual)
                        return;
                }

                // If the cluster parameter value was modified since 'previousTime', do not proceed
                // with updating server cluster parameter.
                if (_isPersistedStateConflictingWithPreviousTime(_doc.getPreviousTime(),
                                                                 persistedClusterParameter)) {
                    _detectedConcurrentUpdate = true;
                    LOGV2_DEBUG(7880300,
                                1,
                                "encountered unexpected 'clusterParameterTime'",
                                "previousTime"_attr = _doc.getPreviousTime()->asTimestamp());

                    return;
                }

                auto catalogManager = ShardingCatalogManager::get(opCtx);
                ShardingLogging::get(opCtx)->logChange(opCtx,
                                                       "setClusterParameter.start",
                                                       NamespaceString::kClusterParametersNamespace,
                                                       _doc.getParameter(),
                                                       kMajorityWriteConcern,
                                                       catalogManager->localConfigShard(),
                                                       catalogManager->localCatalogClient());

                _updateSession(opCtx);
                const auto session = _getCurrentSession();

                {
                    // Ensure the topology is stable so shards added concurrently will
                    // not miss the cluster parameter. Keep it stable until we have
                    // persisted the cluster parameter on the configsvr so that new
                    // shards that get added will see the new cluster parameter.
                    Lock::SharedLock stableTopologyRegion =
                        catalogManager->enterStableTopologyRegion(opCtx);

                    _sendSetClusterParameterToAllShards(opCtx, session, executor);

                    _commit(opCtx);
                }

                ShardingLogging::get(opCtx)->logChange(opCtx,
                                                       "setClusterParameter.end",
                                                       NamespaceString::kClusterParametersNamespace,
                                                       _doc.getParameter(),
                                                       kMajorityWriteConcern,
                                                       catalogManager->localConfigShard(),
                                                       catalogManager->localCatalogClient());
            }));
}

}  // namespace mongo
