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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/drop_indexes_gen.h"
#include "mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/local_catalog/drop_indexes.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(reIndexCrashAfterDrop);

class CmdDropIndexes : public DropIndexesCmdVersion1Gen<CmdDropIndexes> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }
    std::string help() const override {
        return "drop indexes for a collection";
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

        bool supportsRawData() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop index(es) on collection"
                                  << request().getNamespace().toStringForErrorMsg(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(request().getNamespace(),
                                                            ActionType::dropIndex));
        }
        Reply typedRun(OperationContext* opCtx) final {
            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, std::vector<NamespaceString>{ns()});
            return dropIndexes(opCtx,
                               request().getNamespace(),
                               request().getCollectionUUID(),
                               request().getIndex(),
                               timeseries::isRawDataRequest(opCtx, request()));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdDropIndexes).forShard();

class CmdReIndex : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        // Even though reIndex is a standalone-only command, this will return that the command is
        // allowed on secondaries so that it will fail with a more useful error message to the user
        // rather than with a NotWritablePrimary error.
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "re-index a collection (can only be run on a standalone mongod)";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::reIndex)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    CmdReIndex() : BasicCommand("reIndex") {}

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        LOGV2_WARNING(6508600,
                      "The reIndex command is deprecated. For more information, see "
                      "https://mongodb.com/docs/manual/reference/command/reIndex/");

        const NamespaceString toReIndexNss =
            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);

        boost::optional<ReplicaSetDDLTracker::ScopedReplicaSetDDL> scopedReplicaSetDDL{
            boost::in_place_init, opCtx, std::vector<NamespaceString>{toReIndexNss}};

        LOGV2(20457, "CMD reIndex", logAttrs(toReIndexNss));

        if (repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
            uasserted(
                ErrorCodes::IllegalOperation,
                str::stream()
                    << "reIndex is only allowed on a standalone mongod instance. Cannot reIndex '"
                    << toReIndexNss.toStringForErrorMsg() << "' while replication is active");
        }

        auto acquisition = [&] {
            auto collOrViewAcquisition = acquireCollectionOrView(
                opCtx,
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, toReIndexNss, AcquisitionPrerequisites::OperationType::kWrite),
                MODE_X);
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    "can't re-index a view",
                    !collOrViewAcquisition.isView());
            return CollectionAcquisition(std::move(collOrViewAcquisition));
        }();
        uassert(ErrorCodes::NamespaceNotFound, "collection does not exist", acquisition.exists());

        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            acquisition.uuid());

        AutoStatsTracker statsTracker(opCtx,
                                      toReIndexNss,
                                      Top::LockType::WriteLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(toReIndexNss.dbName()));

        const auto defaultIndexVersion = IndexDescriptor::getDefaultIndexVersion();

        std::vector<IndexBuildInfo> indexes;
        {
            std::vector<std::string> indexNames;
            writeConflictRetry(opCtx, "listIndexes", toReIndexNss, [&] {
                indexNames.clear();
                acquisition.getCollectionPtr()->getAllIndexes(&indexNames);
            });

            indexes.reserve(indexNames.size());
            for (size_t i = 0; i < indexNames.size(); i++) {
                const std::string& name = indexNames[i];
                BSONObj spec = writeConflictRetry(opCtx, "getIndexSpec", toReIndexNss, [&] {
                    return acquisition.getCollectionPtr()->getIndexSpec(name);
                });

                {
                    BSONObjBuilder bob;

                    for (auto&& indexSpecElem : spec) {
                        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
                        if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
                            // We create a new index specification with the 'v' field set as
                            // 'defaultIndexVersion'.
                            bob.append(IndexDescriptor::kIndexVersionFieldName,
                                       static_cast<int>(defaultIndexVersion));
                        } else {
                            bob.append(indexSpecElem);
                        }
                    }

                    indexes.push_back(
                        IndexBuildInfo(bob.obj(),
                                       *opCtx->getServiceContext()->getStorageEngine(),
                                       dbName,
                                       VersionContext::getDecoration(opCtx)));
                }

                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus =
                    index_key_validate::validateKeyPattern(key, defaultIndexVersion);

                uassertStatusOKWithContext(
                    keyStatus,
                    str::stream()
                        << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation");
            }
        }

        result.appendNumber("nIndexesWas", static_cast<long long>(indexes.size()));


        std::unique_ptr<MultiIndexBlock> indexer = std::make_unique<MultiIndexBlock>();
        indexer->setIndexBuildMethod(IndexBuildMethodEnum::kForeground);
        StatusWith<std::vector<BSONObj>> swIndexesToRebuild(ErrorCodes::UnknownError,
                                                            "Uninitialized");
        writeConflictRetry(opCtx, "dropAllIndexes", toReIndexNss, [&] {
            CollectionWriter collection(opCtx, &acquisition);
            WriteUnitOfWork wunit(opCtx);
            collection.getWritableCollection(opCtx)->getIndexCatalog()->dropAllIndexes(
                opCtx, collection.getWritableCollection(opCtx), true, {});

            swIndexesToRebuild = indexer->init(opCtx,
                                               collection,
                                               indexes,
                                               MultiIndexBlock::kNoopOnInitFn,
                                               MultiIndexBlock::InitMode::SteadyState,
                                               boost::none,
                                               /*generateTableWrites=*/true);
            uassertStatusOK(swIndexesToRebuild.getStatus());
            wunit.commit();
        });

        // The 'indexer' can throw, so ensure build cleanup occurs.
        ScopeGuard abortOnExit([&] {
            CollectionWriter collection(opCtx, &acquisition);
            indexer->abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
        });

        if (MONGO_unlikely(reIndexCrashAfterDrop.shouldFail())) {
            LOGV2(20458, "Exiting because 'reIndexCrashAfterDrop' fail point was set");
            quickExit(ExitCode::abrupt);
        }

        // Explicitly release the scoped DDL object since we have finished initializing the index
        // re-build. Not holding the object for the entire index re-build avoids blocking other
        // operations that are waiting on this DDL operation to complete, such as draining direct
        // connection DDLs during the promotion to sharded.
        scopedReplicaSetDDL.reset();

        // The following function performs its own WriteConflict handling, so don't wrap it in a
        // writeConflictRetry loop.
        uassertStatusOK(indexer->insertAllDocumentsInCollection(opCtx, toReIndexNss));

        uassertStatusOK(indexer->checkConstraints(opCtx, acquisition.getCollectionPtr()));

        writeConflictRetry(opCtx, "commitReIndex", toReIndexNss, [&] {
            CollectionWriter collection(opCtx, &acquisition);
            WriteUnitOfWork wunit(opCtx);
            uassertStatusOK(indexer->commit(opCtx,
                                            collection.getWritableCollection(opCtx),
                                            MultiIndexBlock::kNoopOnCreateEachFn,
                                            MultiIndexBlock::kNoopOnCommitFn));
            wunit.commit();
        });
        abortOnExit.dismiss();

        result.append("nIndexes", static_cast<int>(swIndexesToRebuild.getValue().size()));
        result.append("indexes", swIndexesToRebuild.getValue());

        return true;
    }
};
MONGO_REGISTER_COMMAND(CmdReIndex).forShard();

}  // namespace
}  // namespace mongo
