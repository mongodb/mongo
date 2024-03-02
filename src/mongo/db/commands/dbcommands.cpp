/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/coll_mod_reply_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/drop_database_gen.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/timeseries/timeseries_collmod.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/async_request_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

// Will cause 'CmdDatasize' to hang as it starts executing.
MONGO_FAIL_POINT_DEFINE(hangBeforeDatasizeCount);

class CmdDropDatabase : public DropDatabaseCmdVersion1Gen<CmdDropDatabase> {
public:
    std::string help() const final {
        return "drop (delete) this database";
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }
    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }
    bool allowedWithSecurityToken() const final {
        return true;
    }
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop database '"
                                  << request().getDbName().toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns(), ActionType::dropDatabase));
        }
        Reply typedRun(OperationContext* opCtx) final {
            auto dbName = request().getDbName();
            // disallow dropping the config database
            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
                (dbName == DatabaseName::kConfig)) {
                uasserted(ErrorCodes::IllegalOperation,
                          "Cannot drop 'config' database if mongod started "
                          "with --configsvr");
            }

            if ((repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) &&
                (dbName == DatabaseName::kLocal)) {
                uasserted(ErrorCodes::IllegalOperation,
                          str::stream() << "Cannot drop '" << dbName.toStringForErrorMsg()
                                        << "' database while replication is active");
            }

            if (request().getCommandParameter() != 1) {
                uasserted(5255100, "Have to pass 1 as 'drop' parameter");
            }

            Status status = dropDatabase(opCtx, dbName);
            if (status != ErrorCodes::NamespaceNotFound) {
                uassertStatusOK(status);
            }
            return {};
        }
    };
};
MONGO_REGISTER_COMMAND(CmdDropDatabase).forShard();

/* drop collection */
class CmdDrop : public DropCmdVersion1Gen<CmdDrop> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const final {
        return false;
    }
    std::string help() const final {
        return "drop a collection\n{drop : <collectionName>}";
    }
    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }
    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto ns = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop collection '"
                                  << ns.toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns, ActionType::dropCollection));
        }
        Reply typedRun(OperationContext* opCtx) final {
            if (request().getNamespace().isOplog()) {
                uassert(5255000,
                        "can't drop live oplog while replicating",
                        !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
                auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
                invariant(storageEngine);
                // We use the method supportsRecoveryTimestamp() to detect whether we are using
                // the WiredTiger storage engine, which is currently only storage engine that
                // supports the replSetResizeOplog command.
                uassert(
                    5255001,
                    "can't drop oplog on storage engines that support replSetResizeOplog command",
                    !storageEngine->supportsRecoveryTimestamp());
            }

            // We need to copy the serialization context from the request to the reply object
            Reply reply(
                SerializationContext::stateCommandReply(request().getSerializationContext()));
            uassertStatusOK(
                dropCollection(opCtx,
                               request().getNamespace(),
                               request().getCollectionUUID(),
                               &reply,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(CmdDrop).forShard();

class CmdDataSize final : public TypedCommand<CmdDataSize> {
public:
    using Request = DataSizeCommand;
    using Reply = typename Request::Reply;

    CmdDataSize() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                         ActionType::find));
        }

        NamespaceString ns() const final {
            return request().getCommandParameter();
        }

        Reply typedRun(OperationContext* opCtx) {
            const auto& cmd = request();

            const bool hasMin = cmd.getMin() != boost::none;
            const bool hasMax = cmd.getMax() != boost::none;

            const StringData negation = hasMin ? ""_sd : "not "_sd;
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Max must " << negation << "be set if min is " << negation
                                  << "set.",
                    hasMin == hasMax);

            Timer timer;
            NamespaceString nss = ns();
            auto min = cmd.getMin().get_value_or({});
            auto max = cmd.getMax().get_value_or({});
            auto keyPattern = cmd.getKeyPattern().get_value_or({});
            const bool estimate = cmd.getEstimate();

            Reply reply;

            AutoGetCollectionForReadCommand autoColl(opCtx, nss);
            const auto& collection = autoColl.getCollection();

            if (!collection) {
                // Collection does not exist
                reply.setNumObjects(0);
                reply.setSize(0);
                reply.setMillis(timer.millis());
                return reply;
            }

            if (collection.isSharded_DEPRECATED()) {
                const auto& shardKeyPattern = collection.getShardKeyPattern();
                uassert(ErrorCodes::BadValue,
                        "keyPattern must be empty or must be an object that equals the shard key",
                        keyPattern.isEmpty() ||
                            (SimpleBSONObjComparator::kInstance.evaluate(shardKeyPattern.toBSON() ==
                                                                         keyPattern)));

                uassert(ErrorCodes::BadValue,
                        str::stream() << "min value " << min << " does not have shard key",
                        min.isEmpty() || shardKeyPattern.isShardKey(min));
                min = shardKeyPattern.normalizeShardKey(min);

                uassert(ErrorCodes::BadValue,
                        str::stream() << "max value " << max << " does not have shard key",
                        max.isEmpty() || shardKeyPattern.isShardKey(max));
                max = shardKeyPattern.normalizeShardKey(max);
            }

            const long long numRecords = collection->numRecords(opCtx);
            reply.setNumObjects(numRecords);

            if (numRecords == 0) {
                reply.setSize(0);
                reply.setMillis(timer.millis());
                return reply;
            }
            reply.setEstimate(estimate);

            // hasMin/hasMax check above matches presense of params,
            // This test matches presence of significant param values.
            uassert(ErrorCodes::BadValue,
                    "Only one of min or max specified",
                    min.isEmpty() == max.isEmpty());

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
            if (min.isEmpty() && max.isEmpty()) {
                if (estimate) {
                    reply.setSize(static_cast<long long>(collection->dataSize(opCtx)));
                    reply.setMillis(timer.millis());
                    return reply;
                }
                exec = InternalPlanner::collectionScan(
                    opCtx, &collection, PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            } else {
                if (keyPattern.isEmpty()) {
                    // if keyPattern not provided, try to infer it from the fields in 'min'
                    keyPattern = Helpers::inferKeyPattern(min);
                }

                const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                                   collection,
                                                                   keyPattern,
                                                                   /*requireSingleKey=*/true);

                uassert(ErrorCodes::OperationFailed,
                        "Couldn't find valid index containing key pattern",
                        shardKeyIdx);

                // If both min and max non-empty, append MinKey's to make them fit chosen index
                KeyPattern kp(shardKeyIdx->keyPattern());
                min = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
                max = Helpers::toKeyFormat(kp.extendRangeBound(max, false));

                exec = InternalPlanner::shardKeyIndexScan(opCtx,
                                                          &collection,
                                                          *shardKeyIdx,
                                                          min,
                                                          max,
                                                          BoundInclusion::kIncludeStartKeyOnly,
                                                          PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
            }

            CurOpFailpointHelpers::waitWhileFailPointEnabled(
                &hangBeforeDatasizeCount, opCtx, "hangBeforeDatasizeCount", []() {});


            const auto maxSize = cmd.getMaxSize();
            const auto maxObjects = cmd.getMaxObjects();

            std::remove_const_t<decltype(maxSize)> size = 0;
            std::remove_const_t<decltype(size)> avgObjSize =
                collection->dataSize(opCtx) / numRecords;
            std::remove_const_t<decltype(maxObjects)> numObjects = 0;

            try {
                RecordId loc;
                while (PlanExecutor::ADVANCED ==
                       exec->getNext(static_cast<BSONObj*>(nullptr), &loc)) {
                    if (estimate) {
                        size += avgObjSize;
                    } else {
                        size += collection->getRecordStore()->dataFor(opCtx, loc).size();
                    }

                    ++numObjects;

                    if ((maxSize && (size > maxSize)) ||
                        (maxObjects && (numObjects > maxObjects))) {
                        reply.setMaxReached(true);
                        break;
                    }
                }
            } catch (DBException& ex) {
                LOGV2_WARNING(23801,
                              "Internal error while reading",
                              logAttrs(nss),
                              "error"_attr = ex.toStatus());
                ex.addContext("Executor error while reading during dataSize command");
                throw;
            }

            reply.setSize(size);
            reply.setNumObjects(numObjects);
            reply.setMillis(timer.millis());
            return reply;
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const final {
        return Request::kCommandDescription.toString();
    }
};
MONGO_REGISTER_COMMAND(CmdDataSize).forShard();

Rarely _collStatsSampler;

class CmdCollStats final : public TypedCommand<CmdCollStats> {
public:
    using Request = CollStatsCommand;

    CmdCollStats() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    std::string help() const final {
        return Request::kCommandDescription.toString();
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto as = AuthorizationSession::get(opCtx->getClient());
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    as->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                         ActionType::collStats));
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) final {
            // Critical to monitoring and observability, categorize the command as immediate
            // priority.
            ScopedAdmissionPriority skipAdmissionControl(opCtx,
                                                         AdmissionContext::Priority::kExempt);

            if (_collStatsSampler.tick())
                LOGV2_WARNING(7024600,
                              "The collStats command is deprecated. For more information, see "
                              "https://dochub.mongodb.org/core/collStats-deprecated");

            const auto nss = ns();
            uassert(
                ErrorCodes::OperationFailed, "No collection name specified", !nss.coll().empty());

            auto result = reply->getBodyBuilder();
            // We need to use the serialization context from the request when calling
            // NamespaceStringUtil to build the reply.
            auto serializationCtx =
                SerializationContext::stateCommandReply(request().getSerializationContext());
            result.append("ns", NamespaceStringUtil::serialize(nss, serializationCtx));

            const auto& spec = request().getStorageStatsSpec();
            Status status =
                appendCollectionStorageStats(opCtx, nss, spec, serializationCtx, &result);
            if (!status.isOK() && (status.code() != ErrorCodes::NamespaceNotFound)) {
                uassertStatusOK(status);  // throws
            }
        }
    };
};
MONGO_REGISTER_COMMAND(CmdCollStats).forShard();

class CollectionModCommand : public TypedCommand<CollectionModCommand> {
public:
    using Request = CollMod;
    using Reply = CollModReply;

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "Sets collection options.\n"
               "Example: { collMod: 'foo', viewOn: 'bar'} "
               "Example: { collMod: 'foo', index: {keyPattern: {a: 1}, expireAfterSeconds: 600} "
               "Example: { collMod: 'foo', index: {name: 'bar', expireAfterSeconds: 600} }\n";
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &Request::kAuthorizationContract;
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(auth::checkAuthForCollMod(opCtx,
                                                      AuthorizationSession::get(opCtx->getClient()),
                                                      request().getNamespace(),
                                                      unparsedRequest().body,
                                                      false,
                                                      request().getSerializationContext()));
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) final {
            const auto& cmd = request();
            const auto& nss = request().getNamespace();
            // Targeting the underlying buckets collection directly would make the time-series
            // Collection out of sync with the time-series view document. Additionally, we want to
            // ultimately obscure/hide the underlying buckets collection from the user, so we're
            // disallowing targetting it.
            uassert(ErrorCodes::InvalidNamespace,
                    "collMod on a time-series collection's underlying buckets collection is not "
                    "supported.",
                    !nss.isTimeseriesBucketsCollection());


            // Updating granularity on sharded time-series collections is not allowed.
            auto catalogClient =
                Grid::get(opCtx)->isInitialized() ? Grid::get(opCtx)->catalogClient() : nullptr;
            if (catalogClient && cmd.getTimeseries() && cmd.getTimeseries()->getGranularity()) {
                auto bucketNss = nss.isTimeseriesBucketsCollection()
                    ? nss
                    : nss.makeTimeseriesBucketsNamespace();
                try {
                    auto coll = catalogClient->getCollection(opCtx, bucketNss);
                    uassert(ErrorCodes::NotImplemented,
                            str::stream()
                                << "Cannot update granularity of a sharded time-series collection.",
                            !coll.getTimeseriesFields());
                } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // Collection is not sharded, skip check.
                }
            }

            if (cmd.getValidator() || cmd.getValidationLevel() || cmd.getValidationAction()) {
                // Check for config.settings in the user command since a validator is allowed
                // internally on this collection but the user may not modify the validator.
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "Document validators not allowed on system collection "
                                      << nss.toStringForErrorMsg(),
                        nss != NamespaceString::kConfigSettingsNamespace);
            }

            // We do not use the serialization context for reply object serialization as the reply
            // object doesn't contain any nss or dbName structures.
            auto result = reply->getBodyBuilder();
            uassertStatusOK(timeseries::processCollModCommandWithTimeSeriesTranslation(
                opCtx, nss, cmd, true, &result));

            // Only validate results in test mode so that we don't expose users to errors if we
            // construct an invalid reply.
            if (getTestCommandsEnabled()) {
                validateResult(result.asTempObj());
            }
        }

        void validateResult(const BSONObj& resultObj) {
            auto reply = Reply::parse(IDLParserContext("CollModReply"), resultObj);
            coll_mod_reply_validation::validateReply(reply);
        }
    };
};
MONGO_REGISTER_COMMAND(CollectionModCommand).forShard();

class CmdDbStats final : public TypedCommand<CmdDbStats> {
public:
    using Request = DBStatsCommand;
    using Reply = typename Request::Reply;

    CmdDbStats() : TypedCommand(Request::kCommandName, Request::kCommandAlias) {}

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto as = AuthorizationSession::get(opCtx->getClient());
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                as->isAuthorizedForActionsOnResource(
                    ResourcePattern::forDatabaseName(request().getDbName()), ActionType::dbStats));
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) {
            // Critical to monitoring and observability, categorize the command as immediate
            // priority.
            ScopedAdmissionPriority skipAdmissionControl(opCtx,
                                                         AdmissionContext::Priority::kExempt);

            const auto& cmd = request();
            const auto& dbname = cmd.getDbName();

            uassert(
                ErrorCodes::BadValue, "Scale factor must be greater than zero", cmd.getScale() > 0);

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid db name: " << dbname.toStringForErrorMsg(),
                    DatabaseName::isValid(dbname, DatabaseName::DollarInDbNameBehavior::Allow));

            {
                CurOp::get(opCtx)->ensureStarted();
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setNS_inlock(dbname);
            }

            AutoGetDb autoDb(opCtx, dbname, MODE_IS);
            Database* db = autoDb.getDb();

            // We need to copy the serialization context from the request to the reply object
            Reply reply(SerializationContext::stateCommandReply(cmd.getSerializationContext()));
            reply.setDB(DatabaseNameUtil::serialize(dbname, reply.getSerializationContext()));

            if (!db) {
                // This preserves old behavior where we used to create an empty database even when
                // the database was accessed for a read. Changing this behavior would impact users
                // that have learned to depend on it, so we continue to support it. Ensure that
                // these fields match exactly the fields in `DatabaseImpl::getStats`.
                reply.setCollections(0);
                reply.setViews(0);
                reply.setObjects(0);
                reply.setAvgObjSize(0);
                reply.setDataSize(0);
                reply.setStorageSize(0);
                reply.setIndexes(0);
                reply.setIndexSize(0);
                reply.setTotalSize(0);
                reply.setScaleFactor(cmd.getScale());

                if (cmd.getFreeStorage()) {
                    reply.setFreeStorageSize(0);
                    reply.setIndexFreeStorageSize(0);
                    reply.setTotalFreeStorageSize(0);
                }

                if (!getGlobalServiceContext()->getStorageEngine()->isEphemeral()) {
                    reply.setFsUsedSize(0);
                    reply.setFsTotalSize(0);
                }

            } else {
                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->enter_inlock(
                        dbname, CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(dbname));
                }

                db->getStats(opCtx, &reply, cmd.getFreeStorage(), cmd.getScale());
            }

            return reply;
        }
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    std::string help() const final {
        return Request::kCommandDescription.toString();
    }
};
MONGO_REGISTER_COMMAND(CmdDbStats).forShard();

// Provides the means to asynchronously run `buildinfo` commands.
class BuildInfoExecutor final : public AsyncRequestExecutor {
public:
    BuildInfoExecutor() : AsyncRequestExecutor("BuildInfoExecutor") {}

    Status handleRequest(std::shared_ptr<RequestExecutionContext> rec) {
        // Critical to observability and diagnosability, categorize as immediate priority.
        ScopedAdmissionPriority skipAdmissionControl(rec->getOpCtx(),
                                                     AdmissionContext::Priority::kExempt);

        auto result = rec->getReplyBuilder()->getBodyBuilder();
        VersionInfoInterface::instance().appendBuildInfo(&result);
        appendStorageEngineList(rec->getOpCtx()->getServiceContext(), &result);
        return Status::OK();
    }

    static BuildInfoExecutor* get(ServiceContext* svc);
};

const auto getBuildInfoExecutor = ServiceContext::declareDecoration<BuildInfoExecutor>();
BuildInfoExecutor* BuildInfoExecutor::get(ServiceContext* svc) {
    return const_cast<BuildInfoExecutor*>(&getBuildInfoExecutor(svc));
}

const auto buildInfoExecutorRegisterer = ServiceContext::ConstructorActionRegisterer{
    "BuildInfoExecutor",
    [](ServiceContext* ctx) { getBuildInfoExecutor(ctx).start(); },
    [](ServiceContext* ctx) {
        getBuildInfoExecutor(ctx).stop();
    }};

class CmdBuildInfo : public BasicCommand {
public:
    CmdBuildInfo() : BasicCommand("buildInfo", "buildinfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    std::string help() const final {
        return "get version #, etc.\n"
               "{ buildinfo:1 }";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& jsobj,
             BSONObjBuilder& result) final {
        // Critical to monitoring and observability, categorize the command as immediate
        // priority.
        ScopedAdmissionPriority skipAdmissionControl(opCtx, AdmissionContext::Priority::kExempt);
        VersionInfoInterface::instance().appendBuildInfo(&result);
        appendStorageEngineList(opCtx->getServiceContext(), &result);
        return true;
    }

    Future<void> runAsync(std::shared_ptr<RequestExecutionContext> rec, const DatabaseName&) final {
        auto opCtx = rec->getOpCtx();
        return BuildInfoExecutor::get(opCtx->getServiceContext())->schedule(std::move(rec));
    }
};
MONGO_REGISTER_COMMAND(CmdBuildInfo).forShard();

}  // namespace
}  // namespace mongo
