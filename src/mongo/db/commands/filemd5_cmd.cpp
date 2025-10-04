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


#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_collection.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/md5.h"
#include "mongo/util/namespace_string_util.h"

#include <memory>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

// Failpoint for making filemd5 hang.
MONGO_FAIL_POINT_DEFINE(waitInFilemd5DuringManualYield);

class CmdFileMD5 : public BasicCommand {
public:
    CmdFileMD5() : BasicCommand("filemd5") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return " example: { filemd5 : ObjectId(aaaaaaa) , root : \"fs\" }";
    }


    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        std::string collectionName;
        if (const auto rootElt = cmdObj["root"]) {
            uassert(ErrorCodes::InvalidNamespace,
                    "'root' must be of type String",
                    rootElt.type() == BSONType::string);
            collectionName = rootElt.str();
        }
        if (collectionName.empty())
            collectionName = "fs";
        collectionName += ".chunks";
        return NamespaceStringUtil::deserialize(dbName, collectionName);
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::find)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& jsobj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, jsobj));

        md5digest d;
        md5_state_t st;
        md5_init_state(&st);

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
                        (stateElem.type() == BSONType::binData));

                int len;
                const char* data = stateElem.binDataClean(len);
                massert(16247, "md5 state not correct size", len == sizeof(st));
                memcpy(&st, data, sizeof(st));
            }
            n = jsobj["startAt"].safeNumberInt();
        }

        BSONObj query = BSON("files_id" << jsobj["filemd5"] << "n" << GTE << n);
        BSONObj sort = BSON("files_id" << 1 << "n" << 1);

        return writeConflictRetry(opCtx, "filemd5", NamespaceString(dbName), [&] {
            auto findCommand = std::make_unique<FindCommandRequest>(nss);
            findCommand->setFilter(query.getOwned());
            findCommand->setSort(sort.getOwned());

            auto statusWithCQ = CanonicalQuery::make(
                {.expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
                 .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
            if (!statusWithCQ.isOK()) {
                uasserted(17240, "Can't canonicalize query " + query.toString());
                return false;
            }
            auto cq = std::move(statusWithCQ.getValue());

            // Check shard version at startup.
            // This will throw before we've done any work if shard version is outdated
            // We drop and re-acquire these locks every document because md5'ing is expensive
            auto ctx = acquireCollection(opCtx,
                                         CollectionAcquisitionRequest::fromOpCtx(
                                             opCtx, nss, AcquisitionPrerequisites::kRead),
                                         MODE_IS);

            AutoStatsTracker statsTracker(opCtx,
                                          nss,
                                          Top::LockType::ReadLocked,
                                          AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                          DatabaseProfileSettings::get(opCtx->getServiceContext())
                                              .getDatabaseProfileLevel(nss.dbName()));

            auto exec = uassertStatusOK(getExecutorFind(opCtx,
                                                        MultipleCollectionAccessor{ctx},
                                                        std::move(cq),
                                                        PlanYieldPolicy::YieldPolicy::YIELD_MANUAL,
                                                        QueryPlannerParams::NO_TABLE_SCAN));

            try {
                BSONObj obj;
                while (PlanExecutor::ADVANCED == exec->getNext(&obj, nullptr)) {
                    BSONElement ne = obj["n"];
                    MONGO_verify(ne.isNumber());
                    int myn = ne.safeNumberInt();
                    if (n != myn) {
                        if (partialOk) {
                            break;  // skipped chunk is probably on another shard
                        }
                        LOGV2(
                            20452, "Unexpected chunk", "expected"_attr = n, "observed"_attr = myn);
                        dumpChunks(opCtx, nss, query, sort);
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
                            owned["data"].type() == BSONType::binData);

                    exec->saveState();
                    // UNLOCKED
                    auto yieldResources = yieldTransactionResourcesFromOperationContext(opCtx);

                    int len;
                    const char* data = owned["data"].binDataClean(len);
                    // This is potentially an expensive operation, so do it out of the lock
                    md5_append(&st, (const md5_byte_t*)(data), len);
                    n++;

                    CurOpFailpointHelpers::waitWhileFailPointEnabled(
                        &waitInFilemd5DuringManualYield, opCtx, "waitInFilemd5DuringManualYield");

                    try {
                        // RELOCKED
                        restoreTransactionResourcesToOperationContext(opCtx,
                                                                      std::move(yieldResources));
                    } catch (const ExceptionFor<ErrorCodes::StaleConfig>&) {
                        LOGV2_DEBUG(
                            20453,
                            1,
                            "Chunk metadata changed during filemd5, will retarget and continue");
                        break;
                    }

                    // Now that we have the lock again, we can restore the PlanExecutor.
                    exec->restoreState(nullptr);
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
                    const NamespaceString& ns,
                    const BSONObj& query,
                    const BSONObj& sort) {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{ns};
        findRequest.setFilter(query);
        findRequest.setSort(sort);
        std::unique_ptr<DBClientCursor> c = client.find(std::move(findRequest));
        while (c->more()) {
            LOGV2(20454, "Dumping chunks", "chunk"_attr = c->nextSafe());
        }
    }
};

MONGO_REGISTER_COMMAND(CmdFileMD5).forShard();

}  // namespace
}  // namespace mongo
