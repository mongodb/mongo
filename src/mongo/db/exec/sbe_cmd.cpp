/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/sbe/parser/parser.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"

namespace mongo {
/**
 * A command for manually constructing a SBE query tree and running it.
 *
 * db.runCommand({sbe: "sbe query text"})
 *
 * This command is only for testing/experimentation, and requires the 'enableTestCommands' flag to
 * be turned on.
 */
class SBECommand final : public BasicCommand {
public:
    SBECommand() : BasicCommand("sbe") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        // If SBE is disabled, then also disallow the SBE command.
        uassert(5772200,
                "the SBE command requires the SBE engine to be enabled",
                !internalQueryForceClassicEngine.load());

        // The SBE command may read from multiple collections, but no logic is in place to acquire
        // locks on all of the necessary collections and databases. Therefore, its implementation
        // depends on lock free reads being enabled (which is the case by default).
        uassert(5772201,
                "the SBE command requires lock free reads",
                !storageGlobalParams.disableLockFreeReads);

        long long batchSize;
        uassertStatusOK(CursorRequest::parseCommandCursorOptions(
            cmdObj, query_request_helper::kDefaultBatchSize, &batchSize));

        // Acquire the global lock, acquire a snapshot, and stash our version of the collection
        // catalog. The is necessary because SBE plan executors currently use the external lock
        // policy. Note that this must be done before parsing because the parser may look up
        // collections in the stashed catalog.
        //
        // Unlike the similar 'AutoGetCollection*' variants of this db_raii object, this will not
        // handle re-establishing a view of the catalog which is consistent with the new storage
        // snapshot after a yield. For this reason, the SBE command cannot yield.
        //
        // Also, this will not handle all read concerns. This is ok because the SBE command is
        // experimental and need not support the various read concerns.
        AutoGetDbForReadLockFree autoGet{opCtx, dbname};

        auto env = std::make_unique<sbe::RuntimeEnvironment>();
        sbe::Parser parser(env.get());
        auto root = parser.parse(opCtx, dbname, cmdObj["sbe"].String());
        auto [resultSlot, recordIdSlot] = parser.getTopLevelSlots();

        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        BSONArrayBuilder firstBatch;

        // Create a trivial cannonical query for the 'sbe' command execution.
        NamespaceString nss{dbname};
        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx, std::make_unique<FindCommandRequest>(nss));
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        stage_builder::PlanStageData data{std::move(env)};

        if (resultSlot) {
            data.outputs.set(stage_builder::PlanStageSlots::kResult, *resultSlot);
        }
        if (recordIdSlot) {
            data.outputs.set(stage_builder::PlanStageSlots::kRecordId, *recordIdSlot);
        }

        root->attachToOperationContext(opCtx);
        exec = uassertStatusOK(plan_executor_factory::make(opCtx,
                                                           std::move(cq),
                                                           nullptr,
                                                           {std::move(root), std::move(data)},
                                                           &CollectionPtr::null,
                                                           false, /* returnOwnedBson */
                                                           nss,
                                                           nullptr));
        for (long long objCount = 0; objCount < batchSize; objCount++) {
            BSONObj next;
            PlanExecutor::ExecState state = exec->getNext(&next, nullptr);
            if (state == PlanExecutor::IS_EOF) {
                break;
            }
            invariant(state == PlanExecutor::ADVANCED);

            // If we can't fit this result inside the current batch, then we stash it for later.
            if (!FindCommon::haveSpaceForNext(next, objCount, firstBatch.len())) {
                exec->enqueue(next);
                break;
            }

            firstBatch.append(next);
        }

        if (exec->isEOF()) {
            appendCursorResponseObject(0LL, nss.ns(), firstBatch.arr(), &result);
            return true;
        }

        exec->saveState();
        exec->detachFromOperationContext();
        const auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
            opCtx,
            {std::move(exec),
             nss,
             AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
             APIParameters::get(opCtx),
             opCtx->getWriteConcern(),
             repl::ReadConcernArgs::get(opCtx),
             ReadPreferenceSetting::get(opCtx),
             cmdObj,
             {}});

        appendCursorResponseObject(
            pinnedCursor.getCursor()->cursorid(), nss.ns(), firstBatch.arr(), &result);

        return true;
    }

    // This is a test-only command so shouldn't be enabled in production, but we try to require
    // auth on new test commands anyway, just in case someone enables them by mistake.
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        auto authSession = AuthorizationSession::get(opCtx->getClient());
        if (!authSession->isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }
};

MONGO_REGISTER_TEST_COMMAND(SBECommand);
}  // namespace mongo
