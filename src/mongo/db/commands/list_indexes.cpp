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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/list_indexes.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/list_indexes_gen.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/uuid.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace {

void appendListIndexesCursorReply(CursorId cursorId,
                                  const NamespaceString& cursorNss,
                                  std::vector<mongo::ListIndexesReplyItem>&& firstBatch,
                                  BSONObjBuilder& result) {
    auto reply =
        ListIndexesReply(ListIndexesReplyCursor(cursorId, cursorNss, std::move(firstBatch)));
    reply.serialize(&result);
}

/**
 * Lists the indexes for a given collection.
 * If 'includeBuildUUIDs' is true, then the index build uuid is also returned alongside the index
 * spec for in-progress index builds only.
 *
 * Format:
 * {
 *   listIndexes: <collection name>,
 *   includeBuildUUIDs: <boolean>,
 * }
 *
 * Return format:
 * {
 *   indexes: [
 *     <index>,
 *     ...
 *   ]
 * }
 *
 * Where '<index>' is the index spec if either the index is ready or 'includeBuildUUIDs' is false.
 * If the index is in-progress and 'includeBuildUUIDs' is true then '<index>' has the following
 * format:
 * {
 *   spec: <index spec>,
 *   buildUUID: <index build uuid>
 * }
 */
class CmdListIndexes : public BasicCommand {
public:
    CmdListIndexes() : BasicCommand("listIndexes") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const override {
        return false;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return "list indexes for a collection";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());

        if (!authzSession->isAuthorizedToParseNamespaceElement(cmdObj.firstElement())) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        // Check for the listIndexes ActionType on the database.
        const auto nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(
            opCtx, CommandHelpers::parseNsOrUUID(dbname, cmdObj));
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(nss),
                                                           ActionType::listIndexes)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream()
                          << "Not authorized to list indexes on collection: " << nss.ns());
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        const auto parsed = ListIndexes::parse({"listIndexes"}, cmdObj);
        long long batchSize = std::numeric_limits<long long>::max();
        if (parsed.getCursor() && parsed.getCursor()->getBatchSize()) {
            batchSize = *parsed.getCursor()->getBatchSize();
        }

        NamespaceString nss;
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        std::vector<mongo::ListIndexesReplyItem> firstBatch;
        {
            AutoGetCollectionForReadCommand collection(opCtx, parsed.getNamespaceOrUUID());
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "ns does not exist: " << collection.getNss().ns(),
                    collection);
            nss = collection.getNss();

            auto expCtx = make_intrusive<ExpressionContext>(
                opCtx, std::unique_ptr<CollatorInterface>(nullptr), nss);

            auto indexList = listIndexesInLock(
                opCtx, collection.getCollection(), nss, parsed.getIncludeBuildUUIDs());
            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            for (auto&& indexSpec : indexList) {
                WorkingSetID id = ws->allocate();
                WorkingSetMember* member = ws->get(id);
                member->keyData.clear();
                member->recordId = RecordId();
                member->resetDocument(SnapshotId(), indexSpec.getOwned());
                member->transitionToOwnedObj();
                root->pushBack(id);
            }

            exec =
                uassertStatusOK(plan_executor_factory::make(expCtx,
                                                            std::move(ws),
                                                            std::move(root),
                                                            &CollectionPtr::null,
                                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                                            nss));

            int bytesBuffered = 0;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for later.
                if (!FindCommon::haveSpaceForNext(nextDoc, objCount, bytesBuffered)) {
                    exec->enqueue(nextDoc);
                    break;
                }

                try {
                    firstBatch.push_back(ListIndexesReplyItem::parse(
                        IDLParserErrorContext("ListIndexesReplyItem"), nextDoc));
                } catch (const DBException& exc) {
                    LOGV2_ERROR(5254500,
                                "Could not parse catalog entry while replying to listIndexes",
                                "entry"_attr = nextDoc,
                                "error"_attr = exc);
                    fassertFailed(5254501);
                }
                bytesBuffered += nextDoc.objsize();
            }

            if (exec->isEOF()) {
                appendListIndexesCursorReply(0 /* cursorId */, nss, std::move(firstBatch), result);
                return true;
            }

            exec->saveState();
            exec->detachFromOperationContext();
        }  // Drop collection lock. Global cursor registration must be done without holding any
           // locks.

        const auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
            opCtx,
            {std::move(exec),
             nss,
             AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
             APIParameters::get(opCtx),
             opCtx->getWriteConcern(),
             repl::ReadConcernArgs::get(opCtx),
             cmdObj,
             {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::listIndexes)}});

        appendListIndexesCursorReply(
            pinnedCursor.getCursor()->cursorid(), nss, std::move(firstBatch), result);

        return true;
    }

} cmdListIndexes;

}  // namespace
}  // namespace mongo
