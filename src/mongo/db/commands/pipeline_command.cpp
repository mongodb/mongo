/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/interrupt_status_mongod.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/storage_options.h"

namespace mongo {

namespace {

    /**
     * This is a Runner implementation backed by an aggregation pipeline.
     */
    class PipelineRunner : public Runner {
    public:
        PipelineRunner(intrusive_ptr<Pipeline> pipeline)
            : _pipeline(pipeline)
        {}

        virtual RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
            if (!objOut || dlOut)
                return RUNNER_ERROR;

            if (!_stash.empty()) {
                *objOut = _stash.back();
                _stash.pop_back();
                return RUNNER_ADVANCED;
            }

            if (boost::optional<Document> next = _pipeline->output()->getNext()) {
                *objOut = next->toBson();
                return RUNNER_ADVANCED;
            }

            return RUNNER_EOF;
        }
        virtual bool isEOF() {
            if (!_stash.empty())
                return false;

            if (boost::optional<Document> next = _pipeline->output()->getNext()) {
                _stash.push_back(next->toBson());
                return false;
            }

            return true;
        }
        virtual const string& ns() {
            return _pipeline->getContext()->ns.ns();
        }

        virtual Status getExplainPlan(TypeExplain** explain) const {
            // This should never get called in practice anyway.
            return Status(ErrorCodes::InternalError,
                          "PipelineCursor doesn't implement getExplainPlan");
        }

        // These are all no-ops for PipelineRunners
        virtual void setYieldPolicy(YieldPolicy policy) {}
        virtual void invalidate(const DiskLoc& dl) {}
        virtual void kill() {}
        virtual void saveState() {}
        virtual bool restoreState() { return true; }

        /**
         * Make obj the next object returned by getNext().
         */
        void pushBack(const BSONObj& obj) {
            _stash.push_back(obj);
        }

    private:
        // Things in the _stash sould be returned before pulling items from _pipeline.
        intrusive_ptr<Pipeline> _pipeline;
        vector<BSONObj> _stash;
    };
}

    static bool isCursorCommand(BSONObj cmdObj) {
        BSONElement cursorElem = cmdObj["cursor"];
        if (cursorElem.eoo())
            return false;

        uassert(16954, "cursor field must be missing or an object",
                cursorElem.type() == Object);

        BSONObj cursor = cursorElem.embeddedObject();
        BSONElement batchSizeElem = cursor["batchSize"];
        if (batchSizeElem.eoo()) {
            uassert(16955, "cursor object can't contain fields other than batchSize",
                cursor.isEmpty());
        }
        else {
            uassert(16956, "cursor.batchSize must be a number",
                    batchSizeElem.isNumber());

            // This can change in the future, but for now all negatives are reserved.
            uassert(16957, "Cursor batchSize must not be negative",
                    batchSizeElem.numberLong() >= 0);
        }

        return true;
    }

    static void handleCursorCommand(CursorId id, BSONObj& cmdObj, BSONObjBuilder& result) {
        BSONElement batchSizeElem = cmdObj.getFieldDotted("cursor.batchSize");
        const long long batchSize = batchSizeElem.isNumber()
                                    ? batchSizeElem.numberLong()
                                    : 101; // same as query

        ClientCursorPin pin(id);
        ClientCursor* cursor = pin.c();

        massert(16958, "Cursor shouldn't have been deleted",
                cursor);

        verify(cursor->isAggCursor);
        PipelineRunner* runner = dynamic_cast<PipelineRunner*>(cursor->getRunner());
        verify(runner);
        try {
            const string cursorNs = cursor->ns(); // we need this after cursor may have been deleted

            // can't use result BSONObjBuilder directly since it won't handle exceptions correctly.
            BSONArrayBuilder resultsArray;
            const int byteLimit = MaxBytesToReturnToClientAtOnce;
            BSONObj next;
            for (int objCount = 0; objCount < batchSize; objCount++) {
                // The initial getNext() on a PipelineRunner may be very expensive so we don't do it
                // when batchSize is 0 since that indicates a desire for a fast return.
                if (runner->getNext(&next, NULL) != Runner::RUNNER_ADVANCED) {
                    pin.deleteUnderlying();
                    id = 0;
                    cursor = NULL; // make it an obvious error to use cursor after this point
                    break;
                }

                if (resultsArray.len() + next.objsize() > byteLimit) {
                    // too big. next will be the first doc in the second batch
                    runner->pushBack(next);
                    break;
                }

                resultsArray.append(next);
            }

            if (cursor) {
                // If a time limit was set on the pipeline, remaining time is "rolled over" to the
                // cursor (for use by future getmore ops).
                cursor->setLeftoverMaxTimeMicros( cc().curop()->getRemainingMaxTimeMicros() );
            }

            BSONObjBuilder cursorObj(result.subobjStart("cursor"));
            cursorObj.append("id", id);
            cursorObj.append("ns", cursorNs);
            cursorObj.append("firstBatch", resultsArray.arr());
            cursorObj.done();
        }
        catch (...) {
            // Clean up cursor on way out of scope.
            pin.deleteUnderlying();
            throw;
        }
    }


    class PipelineCommand :
        public Command {
    public:
        PipelineCommand() :Command(Pipeline::commandName) {} // command is called "aggregate"

        // Locks are managed manually, in particular by DocumentSourceCursor.
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return false; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual void help(stringstream &help) const {
            help << "{ pipeline: [ { $operator: {...}}, ... ]"
                 << ", explain: <bool>"
                 << ", allowDiskUsage: <bool>"
                 << ", cursor: {batchSize: <number>}"
                 << " }"
                 << endl
                 << "See http://dochub.mongodb.org/core/aggregation for more details."
                 ;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            Pipeline::addRequiredPrivileges(this, dbname, cmdObj, out);
        }

        virtual bool run(const string &db, BSONObj &cmdObj, int options, string &errmsg,
                         BSONObjBuilder &result, bool fromRepl) {

            string ns = parseNs(db, cmdObj);

            intrusive_ptr<ExpressionContext> pCtx =
                new ExpressionContext(InterruptStatusMongod::status, NamespaceString(ns));
            pCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";

            /* try to parse the command; if this fails, then we didn't run */
            intrusive_ptr<Pipeline> pPipeline = Pipeline::parseCommand(errmsg, cmdObj, pCtx);
            if (!pPipeline.get())
                return false;

#if _DEBUG
            // This is outside of the if block to keep the object alive until the pipeline is finished.
            BSONObj parsed;
            if (!pPipeline->isExplain() && !pCtx->inShard) {
                // Make sure all operations round-trip through Pipeline::toBson()
                // correctly by reparsing every command on DEBUG builds. This is
                // important because sharded aggregations rely on this ability.
                // Skipping when inShard because this has already been through the
                // transformation (and this unsets pCtx->inShard).
                parsed = pPipeline->serialize().toBson();
                pPipeline = Pipeline::parseCommand(errmsg, parsed, pCtx);
                verify(pPipeline);
            }
#endif

            // This does the mongod-specific stuff like creating a cursor
            PipelineD::prepareCursorSource(pPipeline, pCtx);
            pPipeline->stitch();

            if (pPipeline->isExplain()) {
                result << "stages" << Value(pPipeline->writeExplainOps());
                return true; // don't do any actual execution
            }

            if (isCursorCommand(cmdObj)) {
                CursorId id;
                {
                    // Set up cursor
                    Client::ReadContext ctx(ns);
                    ClientCursor* cc = new ClientCursor(new PipelineRunner(pPipeline));
                    cc->isAggCursor = true; // enable special locking and ns deletion behavior
                    id = cc->cursorid();
                }

                handleCursorCommand(id, cmdObj, result);
            }
            else {
                pPipeline->run(result);
            }

            return true;
        }
    } cmdPipeline;

} // namespace mongo
