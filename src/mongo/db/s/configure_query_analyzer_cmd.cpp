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

#include <boost/smart_ptr.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/analyze_shard_key_util.h"
#include "mongo/db/s/ddl_lock_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/configure_query_analyzer_cmd_gen.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr int kMaxSamplesPerSecond = 50;

/**
 * RAII type for the DDL lock. On a sharded cluster, the lock is the DDLLockManager collection lock.
 * On a replica set, the lock is the collection IX lock.
 */
class ScopedDDLLock {
    ScopedDDLLock(const ScopedDDLLock&) = delete;
    ScopedDDLLock& operator=(const ScopedDDLLock&) = delete;

public:
    ScopedDDLLock(OperationContext* opCtx, const NamespaceString& nss) {
        if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
            // Acquire the DDL lock to serialize with other DDL operations. It also makes sure that
            // we are targeting the primary shard for this database.
            _collDDLLock.emplace(opCtx, nss, "configureQueryAnalyzer", MODE_X);
        } else {
            _autoColl.emplace(opCtx,
                              nss,
                              MODE_IX,
                              AutoGetCollection::Options{}.viewMode(
                                  auto_get_collection::ViewMode::kViewsPermitted));
        }
    }

private:
    boost::optional<DDLLockManager::ScopedCollectionDDLLock> _collDDLLock;
    boost::optional<AutoGetCollection> _autoColl;
};

/**
 * Waits for the system last opTime to be majority committed.
 */
void waitUntilMajorityLastOpTime(OperationContext* opCtx) {
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajorityForWrite(opCtx->getServiceContext(),
                                   repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                                   CancellationToken::uncancelable())
        .get();
}

class ConfigureQueryAnalyzerCmd : public TypedCommand<ConfigureQueryAnalyzerCmd> {
public:
    using Request = ConfigureQueryAnalyzer;
    using Response = ConfigureQueryAnalyzerResponse;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "configureQueryAnalyzer command is not supported on a standalone mongod",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
            uassert(ErrorCodes::IllegalOperation,
                    "configureQueryAnalyzer command is not supported on a multitenant replica set",
                    !gMultitenancySupport);
            uassert(ErrorCodes::IllegalOperation,
                    "Cannot run configureQueryAnalyzer command directly against a shardsvr mongod",
                    serverGlobalParams.clusterRole.has(ClusterRole::None) ||
                        isInternalClient(opCtx) || TestingProctor::instance().isEnabled());

            const auto& nss = ns();
            const auto mode = request().getMode();
            const auto samplesPerSec = request().getSamplesPerSecond();
            const auto newConfig = request().getConfiguration();

            uassertStatusOK(validateNamespace(nss));
            if (mode == QueryAnalyzerModeEnum::kOff) {
                uassert(ErrorCodes::InvalidOptions,
                        "Cannot specify 'samplesPerSecond' when 'mode' is \"off\"",
                        !samplesPerSec);
            } else {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "'samplesPerSecond' must be specified when 'mode' is not \"off\"",
                        samplesPerSec);
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'samplesPerSecond' must be greater than 0",
                        *samplesPerSec > 0);
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'samplesPerSecond' must be less than or equal to "
                                      << kMaxSamplesPerSecond,
                        (*samplesPerSec <= kMaxSamplesPerSecond) ||
                            TestingProctor::instance().isEnabled());
            }

            // Take the DDL lock to serialize this command with DDL commands.
            boost::optional<ScopedDDLLock> ddlLock;
            ddlLock.emplace(opCtx, nss);

            // Wait for the metadata for this collection in the CollectionCatalog to be majority
            // committed before validating its options and persisting the configuration.
            waitUntilMajorityLastOpTime(opCtx);
            auto collUuid = uassertStatusOK(validateCollectionOptions(opCtx, nss));

            LOGV2(6915001,
                  "Persisting query analyzer configuration",
                  logAttrs(nss),
                  "collectionUUID"_attr = collUuid,
                  "mode"_attr = mode,
                  "samplesPerSecond"_attr = samplesPerSec);

            write_ops::FindAndModifyCommandRequest request(
                NamespaceString::kConfigQueryAnalyzersNamespace);

            using doc = QueryAnalyzerDocument;

            auto currentTime = opCtx->getServiceContext()->getFastClockSource()->now();
            if (mode == QueryAnalyzerModeEnum::kOff) {
                request.setUpsert(false);
                // If the mode is 'off', do not perform the update since that would overwrite the
                // existing stop time.
                request.setQuery(BSON(
                    doc::kNsFieldName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                    << doc::kModeFieldName
                    << BSON("$ne" << QueryAnalyzerMode_serializer(QueryAnalyzerModeEnum::kOff))));

                std::vector<BSONObj> updates;
                updates.push_back(
                    BSON("$set" << BSON(doc::kModeFieldName
                                        << QueryAnalyzerMode_serializer(QueryAnalyzerModeEnum::kOff)
                                        << doc::kStopTimeFieldName << currentTime)));
                request.setUpdate(write_ops::UpdateModification(updates));
            } else {
                request.setUpsert(true);
                request.setQuery(BSON(doc::kNsFieldName << NamespaceStringUtil::serialize(
                                          nss, SerializationContext::stateDefault())));

                std::vector<BSONObj> updates;
                BSONObjBuilder setBuilder;
                setBuilder.appendElements(BSON(
                    doc::kCollectionUuidFieldName
                    << collUuid << doc::kNsFieldName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
                setBuilder.appendElements(newConfig.toBSON());
                // If the mode or collection UUID is different, set a new start time. Otherwise,
                // keep the original start time.
                setBuilder.append(
                    doc::kStartTimeFieldName,
                    BSON("$cond" << BSON(
                             "if" << BSON("$or" << BSON_ARRAY(
                                              BSON("$ne" << BSON_ARRAY(
                                                       ("$" + doc::kModeFieldName)
                                                       << QueryAnalyzerMode_serializer(mode)))
                                              << BSON("$ne" << BSON_ARRAY(
                                                          ("$" + doc::kCollectionUuidFieldName)
                                                          << collUuid))))
                                  << "then" << currentTime << "else"
                                  << ("$" + doc::kStartTimeFieldName))));
                updates.push_back(BSON("$set" << setBuilder.obj()));
                updates.push_back(BSON("$unset" << doc::kStopTimeFieldName));
                request.setUpdate(write_ops::UpdateModification(updates));
            }

            auto writeResult = [&] {
                if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
                    request.setWriteConcern(WriteConcerns::kMajorityWriteConcernNoTimeout.toBSON());

                    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
                    auto swResponse = configShard->runCommandWithFixedRetryAttempts(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        DatabaseName::kConfig,
                        request.toBSON({}),
                        Shard::RetryPolicy::kIdempotent);
                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(swResponse));
                    return write_ops::FindAndModifyCommandReply::parse(
                        IDLParserContext("configureQueryAnalyzer"), swResponse.getValue().response);
                }

                DBDirectClient client(opCtx);
                // It is illegal to wait for replication while holding a lock so instead wait below
                // after releasing the lock.
                request.setWriteConcern(BSONObj());
                return client.findAndModify(request);
            }();

            ddlLock.reset();
            if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
                // Wait for the write above to be majority committed.
                waitUntilMajorityLastOpTime(opCtx);
            }

            Response response;
            response.setNewConfiguration(newConfig);
            if (writeResult.getValue()) {
                auto preImageDoc =
                    doc::parse(IDLParserContext("configureQueryAnalyzer"), *writeResult.getValue());
                if (preImageDoc.getCollectionUuid() == collUuid) {
                    response.setOldConfiguration(preImageDoc.getConfiguration());
                }
            } else if (mode != QueryAnalyzerModeEnum::kOff) {
                LOGV2_WARNING(
                    7724700,
                    "Attempted to disable query sampling but query sampling was not active");
            }

            return response;
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::configureQueryAnalyzer));
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Starts or stops collecting metrics about read and write queries against a "
               "collection.";
    }
};
MONGO_REGISTER_COMMAND(ConfigureQueryAnalyzerCmd).forShard();

}  // namespace

}  // namespace analyze_shard_key
}  // namespace mongo
