/**
 *    Copyright (C) 2014-2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using stdx::make_unique;

namespace {

class ParallelCollectionScanCmd : public BasicCommand {
public:
    struct ExtentInfo {
        ExtentInfo(RecordId dl, size_t s) : diskLoc(dl), size(s) {}
        RecordId diskLoc;
        size_t size;
    };

    ParallelCollectionScanCmd() : BasicCommand("parallelCollectionScan") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }

    bool supportsReadConcern(const std::string& dbName, const BSONObj& cmdObj) const final {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kCommand;
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        ActionSet actions;
        actions.addAction(ActionType::find);
        Privilege p(parseResourcePattern(dbname, cmdObj), actions);
        if (AuthorizationSession::get(client)->isAuthorizedForPrivilege(p))
            return Status::OK();
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const NamespaceString ns(parseNsOrUUID(opCtx, dbname, cmdObj));

        AutoGetCollectionForReadCommand ctx(opCtx, ns);

        Collection* collection = ctx.getCollection();
        if (!collection)
            return appendCommandStatus(result,
                                       Status(ErrorCodes::NamespaceNotFound,
                                              str::stream() << "ns does not exist: " << ns.ns()));

        size_t numCursors = static_cast<size_t>(cmdObj["numCursors"].numberInt());

        if (numCursors == 0 || numCursors > 10000)
            return appendCommandStatus(result,
                                       Status(ErrorCodes::BadValue,
                                              str::stream()
                                                  << "numCursors has to be between 1 and 10000"
                                                  << " was: "
                                                  << numCursors));

        std::vector<std::unique_ptr<RecordCursor>> iterators;
        // Opening multiple cursors on a capped collection and reading them in parallel can produce
        // behavior that is not well defined. This can be removed when support for parallel
        // collection scan on capped collections is officially added. The 'getCursor' function
        // ensures that the cursor returned iterates the capped collection in proper document
        // insertion order.
        if (collection->isCapped()) {
            iterators.push_back(collection->getCursor(opCtx));
            numCursors = 1;
        } else {
            iterators = collection->getManyCursors(opCtx);
            if (iterators.size() < numCursors) {
                numCursors = iterators.size();
            }
        }

        std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
        for (size_t i = 0; i < numCursors; i++) {
            unique_ptr<WorkingSet> ws = make_unique<WorkingSet>();
            unique_ptr<MultiIteratorStage> mis =
                make_unique<MultiIteratorStage>(opCtx, ws.get(), collection);

            // Takes ownership of 'ws' and 'mis'.
            auto statusWithPlanExecutor = PlanExecutor::make(
                opCtx, std::move(ws), std::move(mis), collection, PlanExecutor::YIELD_AUTO);
            invariant(statusWithPlanExecutor.isOK());
            execs.push_back(std::move(statusWithPlanExecutor.getValue()));
        }

        // transfer iterators to executors using a round-robin distribution.
        // TODO consider using a common work queue once invalidation issues go away.
        for (size_t i = 0; i < iterators.size(); i++) {
            auto& planExec = execs[i % execs.size()];
            MultiIteratorStage* mis = checked_cast<MultiIteratorStage*>(planExec->getRootStage());
            mis->addIterator(std::move(iterators[i]));
        }

        {
            BSONArrayBuilder bucketsBuilder;
            for (auto&& exec : execs) {
                // Need to save state while yielding locks between now and getMore().
                exec->saveState();
                exec->detachFromOperationContext();

                // Create and register a new ClientCursor.
                auto pinnedCursor = collection->getCursorManager()->registerCursor(
                    opCtx,
                    {std::move(exec),
                     ns,
                     AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                     opCtx->recoveryUnit()->isReadingFromMajorityCommittedSnapshot(),
                     cmdObj});
                pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(
                    opCtx->getRemainingMaxTimeMicros());

                BSONObjBuilder threadResult;
                appendCursorResponseObject(
                    pinnedCursor.getCursor()->cursorid(), ns.ns(), BSONArray(), &threadResult);
                threadResult.appendBool("ok", 1);

                bucketsBuilder.append(threadResult.obj());
            }
            result.appendArray("cursors", bucketsBuilder.obj());
        }

        return true;
    }
} parallelCollectionScanCmd;

}  // namespace
}  // namespace mongo
