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


#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/drop_indexes_gen.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"

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
    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return request().getNamespace();
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop index(es) on collection"
                                  << request().getNamespace(),
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(request().getNamespace(),
                                                            ActionType::dropIndex));
        }
        Reply typedRun(OperationContext* opCtx) final {
            // If the request namespace refers to a time-series collection, transform the user
            // time-series index request to one on the underlying bucket.
            auto isCommandOnTimeseriesBucketNamespace =
                request().getIsTimeseriesNamespace() && *request().getIsTimeseriesNamespace();
            if (auto options = timeseries::getTimeseriesOptions(
                    opCtx, request().getNamespace(), !isCommandOnTimeseriesBucketNamespace)) {
                auto timeseriesCmd =
                    timeseries::makeTimeseriesDropIndexesCommand(opCtx, request(), *options);
                return dropIndexes(opCtx,
                                   timeseriesCmd.getNamespace(),
                                   request().getCollectionUUID(),
                                   timeseriesCmd.getIndex());
            }

            return dropIndexes(opCtx,
                               request().getNamespace(),
                               request().getCollectionUUID(),
                               request().getIndex());
        }
    };
} cmdDropIndexes;

class CmdReIndex : public ErrmsgCommandDeprecated {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        // Even though reIndex is a standalone-only command, this will return that the command is
        // allowed on secondaries so that it will fail with a more useful error message to the user
        // rather than with a NotWritablePrimary error.
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    std::string help() const override {
        return "re-index a collection (can only be run on a standalone mongod)";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::reIndex);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }
    CmdReIndex() : ErrmsgCommandDeprecated("reIndex") {}

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& jsobj,
                   std::string& errmsg,
                   BSONObjBuilder& result) {
        LOGV2_WARNING(6508600,
                      "The reIndex command is deprecated. For more information, see "
                      "https://mongodb.com/docs/manual/reference/command/reIndex/");

        const NamespaceString toReIndexNss =
            CommandHelpers::parseNsCollectionRequired(dbname, jsobj);

        LOGV2(20457, "CMD: reIndex {namespace}", "CMD reIndex", "namespace"_attr = toReIndexNss);

        if (repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() !=
            repl::ReplicationCoordinator::modeNone) {
            uasserted(
                ErrorCodes::IllegalOperation,
                str::stream()
                    << "reIndex is only allowed on a standalone mongod instance. Cannot reIndex '"
                    << toReIndexNss << "' while replication is active");
        }

        AutoGetCollection autoColl(opCtx, toReIndexNss, MODE_X);
        if (!autoColl) {
            if (CollectionCatalog::get(opCtx)->lookupView(opCtx, toReIndexNss))
                uasserted(ErrorCodes::CommandNotSupportedOnView, "can't re-index a view");
            else
                uasserted(ErrorCodes::NamespaceNotFound, "collection does not exist");
        }

        CollectionWriter collection(opCtx, autoColl);
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            collection->uuid());

        // This is necessary to set up CurOp and update the Top stats.
        OldClientContext ctx(opCtx, toReIndexNss);

        const auto defaultIndexVersion = IndexDescriptor::getDefaultIndexVersion();

        std::vector<BSONObj> all;
        {
            std::vector<std::string> indexNames;
            writeConflictRetry(opCtx, "listIndexes", toReIndexNss.ns(), [&] {
                indexNames.clear();
                collection->getAllIndexes(&indexNames);
            });

            all.reserve(indexNames.size());

            for (size_t i = 0; i < indexNames.size(); i++) {
                const std::string& name = indexNames[i];
                BSONObj spec = writeConflictRetry(opCtx, "getIndexSpec", toReIndexNss.ns(), [&] {
                    return collection->getIndexSpec(name);
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

                    all.push_back(bob.obj());
                }

                const BSONObj key = spec.getObjectField("key");
                const Status keyStatus =
                    index_key_validate::validateKeyPattern(key, defaultIndexVersion);
                if (!keyStatus.isOK()) {
                    errmsg = str::stream()
                        << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation";
                    return false;
                }
            }
        }

        result.appendNumber("nIndexesWas", static_cast<long long>(all.size()));

        std::unique_ptr<MultiIndexBlock> indexer = std::make_unique<MultiIndexBlock>();
        indexer->setIndexBuildMethod(IndexBuildMethod::kForeground);
        StatusWith<std::vector<BSONObj>> swIndexesToRebuild(ErrorCodes::UnknownError,
                                                            "Uninitialized");
        writeConflictRetry(opCtx, "dropAllIndexes", toReIndexNss.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            collection.getWritableCollection(opCtx)->getIndexCatalog()->dropAllIndexes(
                opCtx, collection.getWritableCollection(opCtx), true, {});

            swIndexesToRebuild =
                indexer->init(opCtx, collection, all, MultiIndexBlock::kNoopOnInitFn);
            uassertStatusOK(swIndexesToRebuild.getStatus());
            wunit.commit();
        });

        // The 'indexer' can throw, so ensure build cleanup occurs.
        ScopeGuard abortOnExit([&] {
            indexer->abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
        });

        if (MONGO_unlikely(reIndexCrashAfterDrop.shouldFail())) {
            LOGV2(20458, "Exiting because 'reIndexCrashAfterDrop' fail point was set");
            quickExit(ExitCode::abrupt);
        }

        // The following function performs its own WriteConflict handling, so don't wrap it in a
        // writeConflictRetry loop.
        uassertStatusOK(indexer->insertAllDocumentsInCollection(opCtx, collection.get()));

        uassertStatusOK(indexer->checkConstraints(opCtx, collection.get()));

        writeConflictRetry(opCtx, "commitReIndex", toReIndexNss.ns(), [&] {
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
} cmdReIndex;

}  // namespace
}  // namespace mongo
