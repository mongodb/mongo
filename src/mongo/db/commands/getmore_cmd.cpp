/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    /**
     * A command for running getMore() against an existing cursor registered with a
     * CursorManager.
     *
     * Can be used in combination with any cursor-generating command (e.g. find, aggregate,
     * listIndexes).
     */
    class GetMoreCmd : public Command {
        MONGO_DISALLOW_COPYING(GetMoreCmd);
    public:
        GetMoreCmd() : Command("getMore") { }

        bool isWriteCommandForConfigServer() const override { return false; }

        bool slaveOk() const override { return false; }

        bool slaveOverrideOk() const override { return true; }

        bool maintenanceOk() const override { return false; }

        bool adminOnly() const override { return false; }

        void help(std::stringstream& help) const override {
            help << "retrieve more results from an existing cursor";
        }

        /**
         * A getMore command increments the getMore counter, not the command counter.
         */
        bool shouldAffectCommandCounter() const override { return false; }

        std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
            return GetMoreRequest::parseNs(dbname, cmdObj);
        }

        Status checkAuthForCommand(ClientBasic* client,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj) override {
            StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
            if (!parseStatus.isOK()) {
                return parseStatus.getStatus();
            }
            const GetMoreRequest& request = parseStatus.getValue();

            return client->getAuthorizationSession()->checkAuthForGetMore(request.nss,
                                                                          request.cursorid);
        }

        /**
         * Generates the next batch of results for a ClientCursor.
         *
         * TODO: Do we need to support some equivalent of OP_REPLY responseFlags?
         *
         * TODO: Is it possible to support awaitData?
         */
        bool run(OperationContext* txn,
                 const std::string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 std::string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) override {
            // Counted as a getMore, not as a command.
            globalOpCounters.gotGetMore();

            if (txn->getClient()->isInDirectClient()) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::IllegalOperation,
                                                  "Cannot run getMore command from eval()"));
            }

            StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
            if (!parseStatus.isOK()) {
                return appendCommandStatus(result, parseStatus.getStatus());
            }
            const GetMoreRequest& request = parseStatus.getValue();

            // Depending on the type of cursor being operated on, we hold locks for the whole
            // getMore, or none of the getMore, or part of the getMore.  The three cases in detail:
            //
            // 1) Normal cursor: we lock with "ctx" and hold it for the whole getMore.
            // 2) Cursor owned by global cursor manager: we don't lock anything.  These cursors
            //    don't own any collection state.
            // 3) Agg cursor: we lock with "ctx", then release, then relock with "unpinDBLock" and
            //    "unpinCollLock".  This is because agg cursors handle locking internally (hence the
            //    release), but the pin and unpin of the cursor must occur under the collection
            //    lock. We don't use our AutoGetCollectionForRead "ctx" to relock, because
            //    AutoGetCollectionForRead checks the sharding version (and we want the relock for
            //    the unpin to succeed even if the sharding version has changed).
            //
            // Note that we declare our locks before our ClientCursorPin, in order to ensure that
            // the pin's destructor is called before the lock destructors (so that the unpin occurs
            // under the lock).
            std::unique_ptr<AutoGetCollectionForRead> ctx;
            std::unique_ptr<Lock::DBLock> unpinDBLock;
            std::unique_ptr<Lock::CollectionLock> unpinCollLock;

            CursorManager* cursorManager;
            CursorManager* globalCursorManager = CursorManager::getGlobalCursorManager();
            if (globalCursorManager->ownsCursorId(request.cursorid)) {
                cursorManager = globalCursorManager;
            }
            else {
                ctx.reset(new AutoGetCollectionForRead(txn, request.nss));
                Collection* collection = ctx->getCollection();
                if (!collection) {
                    return appendCommandStatus(result,
                                               Status(ErrorCodes::OperationFailed,
                                                      "collection dropped between getMore calls"));
                }
                cursorManager = collection->getCursorManager();
            }

            ClientCursorPin ccPin(cursorManager, request.cursorid);
            ClientCursor* cursor = ccPin.c();
            if (!cursor) {
                // We didn't find the cursor.
                return appendCommandStatus(result, Status(ErrorCodes::CursorNotFound, str::stream()
                    << "Cursor not found, cursor id: " << request.cursorid));
            }

            if (request.nss.ns() != cursor->ns()) {
                return appendCommandStatus(result, Status(ErrorCodes::Unauthorized, str::stream()
                    << "Requested getMore on namespace '" << request.nss.ns()
                    << "', but cursor belongs to a different namespace"));
            }

            // On early return, get rid of the the cursor.
            ScopeGuard cursorFreer = MakeGuard(&ClientCursorPin::deleteUnderlying, ccPin);

            if (!cursor->hasRecoveryUnit()) {
                // Start using a new RecoveryUnit.
                cursor->setOwnedRecoveryUnit(
                    getGlobalServiceContext()->getGlobalStorageEngine()->newRecoveryUnit());
            }

            // Swap RecoveryUnit(s) between the ClientCursor and OperationContext.
            ScopedRecoveryUnitSwapper ruSwapper(cursor, txn);

            // Reset timeout timer on the cursor since the cursor is still in use.
            cursor->setIdleTime(0);

            // If the operation that spawned this cursor had a time limit set, apply leftover
            // time to this getmore.
            txn->getCurOp()->setMaxTimeMicros(cursor->getLeftoverMaxTimeMicros());
            txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

            if (cursor->isAggCursor()) {
                // Agg cursors handle their own locking internally.
                ctx.reset(); // unlocks
            }

            PlanExecutor* exec = cursor->getExecutor();
            exec->restoreState(txn);

            // TODO: Handle result sets larger than 16MB.
            BSONArrayBuilder nextBatch;
            BSONObj obj;
            PlanExecutor::ExecState state;
            int numResults = 0;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                // Add result to output buffer.
                nextBatch.append(obj);
                numResults++;

                if (enoughForGetMore(request.batchSize, numResults, nextBatch.len())) {
                    break;
                }
            }

            // If we are operating on an aggregation cursor, then we dropped our collection lock
            // earlier and need to reacquire it in order to clean up our ClientCursorPin.
            //
            // TODO: We need to ensure that this relock happens if we release the pin above in
            // response to PlanExecutor::getNext() throwing an exception.
            if (cursor->isAggCursor()) {
                invariant(NULL == ctx.get());
                unpinDBLock.reset(new Lock::DBLock(txn->lockState(), request.nss.db(), MODE_IS));
                unpinCollLock.reset(
                    new Lock::CollectionLock(txn->lockState(), request.nss.ns(), MODE_IS));
            }

            // Fail the command if the PlanExecutor reports execution failure.
            if (PlanExecutor::FAILURE == state) {
                const std::unique_ptr<PlanStageStats> stats(exec->getStats());
                error() << "GetMore executor error, stats: " << Explain::statsToBSON(*stats);
                return appendCommandStatus(result,
                                           Status(ErrorCodes::OperationFailed,
                                                  str::stream() << "GetMore executor error: "
                                                  << WorkingSetCommon::toStatusString(obj)));
            }

            CursorId respondWithId = 0;
            if (shouldSaveCursorGetMore(state, exec, isCursorTailable(cursor))) {
                respondWithId = request.cursorid;

                exec->saveState();

                cursor->setLeftoverMaxTimeMicros(txn->getCurOp()->getRemainingMaxTimeMicros());
                cursor->incPos(numResults);

                if (isCursorTailable(cursor) && state == PlanExecutor::IS_EOF) {
                    // Rather than swapping their existing RU into the client cursor, tailable
                    // cursors should get a new recovery unit.
                    ruSwapper.dismiss();
                }
            }

            Command::appendGetMoreResponseObject(respondWithId, request.nss.ns(), nextBatch.arr(),
                                                 &result);
            if (respondWithId) {
                cursorFreer.Dismiss();
            }
            return true;
        }

    } getMoreCmd;

} // namespace mongo
