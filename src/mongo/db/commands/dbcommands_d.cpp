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

#include <time.h>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/drop_database.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/profile_common.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/storage_stats.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/stale_exception.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

// Failpoint for making filemd5 hang.
MONGO_FAIL_POINT_DEFINE(waitInFilemd5DuringManualYield);

Status _setProfileSettings(OperationContext* opCtx,
                           Database* db,
                           const DatabaseName& dbName,
                           mongo::CollectionCatalog::ProfileSettings newSettings) {
    invariant(db);

    auto currSettings = CollectionCatalog::get(opCtx)->getDatabaseProfileSettings(dbName);

    if (currSettings == newSettings) {
        return Status::OK();
    }

    if (newSettings.level == 0) {
        // No need to create the profile collection.
        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.setDatabaseProfileSettings(dbName, newSettings);
        });
        return Status::OK();
    }

    // Can't support profiling without supporting capped collections.
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
        return Status(ErrorCodes::CommandNotSupported,
                      "the storage engine doesn't support profiling.");
    }

    Status status = createProfileCollection(opCtx, db);
    if (!status.isOK()) {
        return status;
    }

    CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
        catalog.setDatabaseProfileSettings(dbName, newSettings);
    });

    return Status::OK();
}


/**
 * Sets the profiling level, logging/profiling threshold, and logging/profiling sample rate for the
 * given database.
 */
class CmdProfile : public ProfileCmdBase {
public:
    CmdProfile() = default;

protected:
    CollectionCatalog::ProfileSettings _applyProfilingLevel(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const ProfileCmdRequest& request) const final {
        const auto profilingLevel = request.getCommandParameter();

        // The system.profile collection is non-replicated, so writes to it do not cause
        // replication lag. As such, they should be excluded from Flow Control.
        opCtx->setShouldParticipateInFlowControl(false);

        // An invalid profiling level (outside the range [0, 2]) represents a request to read the
        // current profiling level. Similarly, if the request does not include a filter, we only
        // need to read the current filter, if any. If we're not changing either value, then we can
        // acquire a shared lock instead of exclusive.
        const bool readOnly = (profilingLevel < 0 || profilingLevel > 2) && !request.getFilter();
        const LockMode dbMode = readOnly ? MODE_IS : MODE_IX;

        // Accessing system.profile collection should not conflict with oplog application.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());
        AutoGetDb ctx(opCtx, dbName, dbMode);
        Database* db = ctx.getDb();

        // Fetches the database profiling level + filter or the server default if the db does not
        // exist.
        auto oldSettings = CollectionCatalog::get(opCtx)->getDatabaseProfileSettings(dbName);

        if (!readOnly) {
            if (!db) {
                // When setting the profiling level, create the database if it didn't already exist.
                // When just reading the profiling level, we do not create the database.
                auto databaseHolder = DatabaseHolder::get(opCtx);
                db = databaseHolder->openDb(opCtx, dbName);
            }

            auto newSettings = oldSettings;
            if (profilingLevel >= 0 && profilingLevel <= 2) {
                newSettings.level = profilingLevel;
            }
            if (auto filterOrUnset = request.getFilter()) {
                if (auto filter = filterOrUnset->obj) {
                    // filter: <match expression>
                    newSettings.filter = std::make_shared<ProfileFilterImpl>(*filter);
                } else {
                    // filter: "unset"
                    newSettings.filter = nullptr;
                }
            }
            uassertStatusOK(_setProfileSettings(opCtx, db, dbName, newSettings));
        }

        return oldSettings;
    }

} cmdProfile;

class CmdFileMD5 : public BasicCommand {
public:
    CmdFileMD5() : BasicCommand("filemd5") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        std::string collectionName;
        if (const auto rootElt = cmdObj["root"]) {
            uassert(ErrorCodes::InvalidNamespace,
                    "'root' must be of type String",
                    rootElt.type() == BSONType::String);
            collectionName = rootElt.str();
        }
        if (collectionName.empty())
            collectionName = "fs";
        collectionName += ".chunks";
        return NamespaceString(dbname, collectionName).ns();
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), ActionType::find));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss(parseNs(dbname, jsobj));

        md5digest d;
        md5_state_t st;
        md5_init(&st);

        int n = 0;

        bool partialOk = jsobj["partialOk"].trueValue();
        if (partialOk) {
            // WARNING: This code depends on the binary layout of md5_state. It will not be
            // compatible with different md5 libraries or work correctly in an environment with
            // mongod's of different endians. It is ok for mongos to be a different endian since
            // it just passes the buffer through to another mongod.
            BSONElement stateElem = jsobj["md5state"];
            if (!stateElem.eoo()) {
                uassert(50847,
                        str::stream() << "The element that calls binDataClean() must be type of "
                                         "BinData, but type of "
                                      << typeName(stateElem.type()) << " found.",
                        (stateElem.type() == BSONType::BinData));

                int len;
                const char* data = stateElem.binDataClean(len);
                massert(16247, "md5 state not correct size", len == sizeof(st));
                memcpy(&st, data, sizeof(st));
            }
            n = jsobj["startAt"].numberInt();
        }

        BSONObj query = BSON("files_id" << jsobj["filemd5"] << "n" << GTE << n);
        BSONObj sort = BSON("files_id" << 1 << "n" << 1);

        return writeConflictRetry(opCtx, "filemd5", dbname, [&] {
            auto findCommand = std::make_unique<FindCommandRequest>(nss);
            findCommand->setFilter(query.getOwned());
            findCommand->setSort(sort.getOwned());

            auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
            if (!statusWithCQ.isOK()) {
                uasserted(17240, "Can't canonicalize query " + query.toString());
                return false;
            }
            std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

            // Check shard version at startup.
            // This will throw before we've done any work if shard version is outdated
            // We drop and re-acquire these locks every document because md5'ing is expensive
            std::unique_ptr<AutoGetCollectionForReadCommand> ctx(
                new AutoGetCollectionForReadCommand(opCtx, nss));
            const CollectionPtr& coll = ctx->getCollection();

            auto exec = uassertStatusOK(getExecutor(opCtx,
                                                    &coll,
                                                    std::move(cq),
                                                    nullptr /* extractAndAttachPipelineStages */,
                                                    PlanYieldPolicy::YieldPolicy::YIELD_MANUAL,
                                                    QueryPlannerParams::NO_TABLE_SCAN));

            // We need to hold a lock to clean up the PlanExecutor, so make sure we have one when we
            // exit this block. Because we use an AutoGetCollectionForReadCommand and manual
            // yielding, we may throw when trying to re-acquire the lock. For example, this can
            // happen if our operation has been interrupted.
            ON_BLOCK_EXIT([&]() {
                if (ctx) {
                    // We still have the lock. No special action required.
                    return;
                }

                // We need to be careful to not use AutoGetCollection or AutoGetDb here, since we
                // only need the lock to protect potential access to the Collection's CursorManager
                // and those helpers may throw if something has changed since the last time we took
                // a lock. For example, AutoGetCollection will throw if this namespace has since
                // turned into a view and AutoGetDb will throw if the database version is stale.
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
                invariant(dbLock.isLocked(),
                          "Expected lock acquisition to succeed due to UninterruptibleLockGuard");
                Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
                exec.reset();
            });

            try {
                BSONObj obj;
                while (PlanExecutor::ADVANCED == exec->getNext(&obj, nullptr)) {
                    BSONElement ne = obj["n"];
                    verify(ne.isNumber());
                    int myn = ne.numberInt();
                    if (n != myn) {
                        if (partialOk) {
                            break;  // skipped chunk is probably on another shard
                        }
                        LOGV2(20452,
                              "Should have chunk: {expected} have: {observed}",
                              "Unexpected chunk",
                              "expected"_attr = n,
                              "observed"_attr = myn);
                        dumpChunks(opCtx, nss.ns(), query, sort);
                        uassert(10040, "chunks out of order", n == myn);
                    }

                    // make a copy of obj since we access data in it while yielding locks
                    BSONObj owned = obj.getOwned();
                    uassert(50848,
                            str::stream() << "The element that calls binDataClean() must be type "
                                             "of BinData, but type of misisng found. Field name is "
                                             "required",
                            owned["data"]);
                    uassert(50849,
                            str::stream() << "The element that calls binDataClean() must be type "
                                             "of BinData, but type of "
                                          << owned["data"].type() << " found.",
                            owned["data"].type() == BSONType::BinData);

                    exec->saveState();
                    // UNLOCKED
                    ctx.reset();

                    int len;
                    const char* data = owned["data"].binDataClean(len);
                    // This is potentially an expensive operation, so do it out of the lock
                    md5_append(&st, (const md5_byte_t*)(data), len);
                    n++;

                    CurOpFailpointHelpers::waitWhileFailPointEnabled(
                        &waitInFilemd5DuringManualYield, opCtx, "waitInFilemd5DuringManualYield");

                    try {
                        // RELOCKED
                        ctx.reset(new AutoGetCollectionForReadCommand(opCtx, nss));
                    } catch (const StaleConfigException&) {
                        LOGV2_DEBUG(
                            20453,
                            1,
                            "Chunk metadata changed during filemd5, will retarget and continue");
                        break;
                    }

                    // Now that we have the lock again, we can restore the PlanExecutor.
                    exec->restoreState(&ctx->getCollection());
                }
            } catch (DBException& exception) {
                exception.addContext("Executor error during filemd5 command");
                throw;
            }

            if (partialOk)
                result.appendBinData("md5state", sizeof(st), BinDataGeneral, &st);

            // This must be *after* the capture of md5state since it mutates st
            md5_finish(&st, d);

            result.append("numChunks", n);
            result.append("md5", digestToString(d));

            return true;
        });
    }

    void dumpChunks(OperationContext* opCtx,
                    const std::string& ns,
                    const BSONObj& query,
                    const BSONObj& sort) {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{NamespaceString{ns}};
        findRequest.setFilter(query);
        findRequest.setSort(sort);
        std::unique_ptr<DBClientCursor> c = client.find(std::move(findRequest));
        while (c->more()) {
            LOGV2(20454, "Chunk: {chunk}", "Dumping chunks", "chunk"_attr = c->nextSafe());
        }
    }

} cmdFileMD5;

}  // namespace
}  // namespace mongo
