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
#include "mongo/db/ops/query.h"

namespace mongo {

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

        // Using limited cursor API that ignores many edge cases. Should be sufficient for commands.
        ClientCursorPin pin(id);
        ClientCursor* cursor = pin.c();

        massert(16958, "Cursor shouldn't have been deleted",
                cursor);

        // Make sure this cursor won't disappear on us
        fassert(16959, !cursor->c()->shouldDestroyOnNSDeletion());
        fassert(16960, !cursor->c()->requiresLock());

        try {
            const string cursorNs = cursor->ns(); // we need this after cursor may have been deleted

            // can't use result BSONObjBuilder directly since it won't handle exceptions correctly.
            BSONArrayBuilder resultsArray;
            const int byteLimit = MaxBytesToReturnToClientAtOnce;
            for (int objCount = 0; objCount < batchSize && cursor->ok(); objCount++) {
                BSONObj current = cursor->current();
                if (resultsArray.len() + current.objsize() > byteLimit)
                    break; // too big. current will be the first doc in the second batch

                resultsArray.append(current);
                cursor->advance();
            }

            // The initial ok() on a cursor may be very expensive so we don't do it when batchSize
            // is 0 since that indicates a desire for a fast return.
            if (batchSize != 0 && !cursor->ok()) {
                // There is no more data. Kill the cursor.
                pin.release();
                ClientCursor::erase(id);
                id = 0;
                cursor = NULL; // make it an obvious error to use cursor after this point
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
            pin.release();
            ClientCursor::erase(id);
            throw;
        }
    }


    class PipelineCursor : public Cursor {
    public:
        PipelineCursor(intrusive_ptr<Pipeline> pipeline)
            : _pipeline(pipeline)
            , _started(false)
            , _done(false)
        {}

        // "core" cursor protocol
        virtual bool ok() {
            if (!_started) {
                _started = true;
                getNext();
            }
            return !_done;
        }
        virtual bool advance() {
            if (!_started) {
                _started = true;
                // skip first result
                getNext();
            }

            getNext();
            return !_done;
        }
        virtual BSONObj current() {
            verify(ok());
            return _currentObj;
        }

        virtual bool requiresLock() { return false; }
        virtual bool shouldDestroyOnNSDeletion() { return false; }

        virtual Record* _current() { return NULL; }
        virtual DiskLoc currLoc() { return DiskLoc(); }
        virtual DiskLoc refLoc() { return DiskLoc(); }
        virtual bool supportGetMore() { return true; }
        virtual bool supportYields() { return false; } // has wrong semantics
        virtual bool getsetdup(DiskLoc loc) { return false; } // we don't generate dups
        virtual bool isMultiKey() const { return false; }
        virtual bool modifiedKeys() const { return false; }
        virtual string toString() { return "Aggregate_Cursor"; }

        // These probably won't be needed once aggregation supports it's own explain.
        virtual long long nscanned() { return 0; }
        virtual void explainDetails( BSONObjBuilder& b ) { return; }
    private:
        const DocumentSource* iterator() const { return _pipeline->output(); }
        DocumentSource* iterator() { return _pipeline->output(); }

        void getNext() {
            if (boost::optional<Document> result = iterator()->getNext()) {
                _currentObj = result->toBson();
            }
            else {
                _done = true;
            }
        }

        intrusive_ptr<Pipeline> _pipeline;
        bool _started;
        bool _done;
        BSONObj _currentObj;
    };

    class PipelineCommand :
        public Command {
    public:
        PipelineCommand() :Command(Pipeline::commandName) {} // command is called "aggregate"

        // Locks are managed manually, in particular by DocumentSourceCursor.
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual void help(stringstream &help) const {
            help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            Pipeline::addRequiredPrivileges(dbname, cmdObj, out);
        }

        virtual bool run(const string &db, BSONObj &cmdObj, int options, string &errmsg,
                         BSONObjBuilder &result, bool fromRepl) {

            string ns = parseNs(db, cmdObj);

            intrusive_ptr<ExpressionContext> pCtx =
                new ExpressionContext(InterruptStatusMongod::status, NamespaceString(ns));

            /* try to parse the command; if this fails, then we didn't run */
            intrusive_ptr<Pipeline> pPipeline = Pipeline::parseCommand(errmsg, cmdObj, pCtx);
            if (!pPipeline.get())
                return false;

            if (pPipeline->getSplitMongodPipeline()) {
                // This is only used in testing
                return executeSplitPipeline(result, errmsg, ns, db, pPipeline, pCtx);
            }

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
            PipelineD::prepareCursorSource(pPipeline, nsToDatabase(ns), pCtx);
            pPipeline->stitch();

            if (isCursorCommand(cmdObj)) {
                CursorId id;
                {
                    // Set up cursor
                    Client::ReadContext ctx(ns);
                    shared_ptr<Cursor> cursor(new PipelineCursor(pPipeline));
                    // cc will be owned by cursor manager
                    ClientCursor* cc = new ClientCursor(0, cursor, ns, cmdObj.getOwned());
                    id = cc->cursorid();
                }

                handleCursorCommand(id, cmdObj, result);
            }
            else {
                pPipeline->run(result);
            }

            if (DocumentSourceOut* out = dynamic_cast<DocumentSourceOut*>(pPipeline->output())) {
                result.append("outputNs", out->getOutputNs());
            }

            return true;
        }

    private:
        /*
          Execute the pipeline for the explain.  This is common to both the
          locked and unlocked code path.  However, the results are different.
          For an explain, with no lock, it really outputs the pipeline
          chain rather than fetching the data.
         */
        bool executeSplitPipeline(BSONObjBuilder& result, string& errmsg,
                                  const string& ns, const string& db,
                                  intrusive_ptr<Pipeline>& pPipeline,
                                  intrusive_ptr<ExpressionContext>& pCtx) {
            /* setup as if we're in the router */
            pCtx->inRouter = true;

            /*
            Here, we'll split the pipeline in the same way we would for sharding,
            for testing purposes.

            Run the shard pipeline first, then feed the results into the remains
            of the existing pipeline.

            Start by splitting the pipeline.
            */
            intrusive_ptr<Pipeline> pShardSplit = pPipeline->splitForSharded();

            // Write the split pipeline as we would in order to transmit it to the shard servers.
            Document shardCmd = pShardSplit->serialize();

            DEV log() << "\n---- shardDescription\n" << shardCmd.toString() << "\n----\n";

            // for debugging purposes, show what the pipeline now looks like
            DEV log() << "\n---- pipelineDescription\n" << pPipeline->serialize() << "\n----\n";

            /* on the shard servers, create the local pipeline */
            intrusive_ptr<ExpressionContext> pShardCtx =
                new ExpressionContext(InterruptStatusMongod::status, NamespaceString(ns));
            BSONObj shardObj = shardCmd.toBson();
            intrusive_ptr<Pipeline> pShardPipeline(
                Pipeline::parseCommand(errmsg, shardObj, pShardCtx));
            if (!pShardPipeline.get()) {
                return false;
            }

            PipelineD::prepareCursorSource(pShardPipeline, nsToDatabase(ns), pCtx);

            /* run the shard pipeline */
            BSONObjBuilder shardResultBuilder;
            string shardErrmsg;
            pShardPipeline->stitch();
            pShardPipeline->run(shardResultBuilder);
            BSONObj shardResult(shardResultBuilder.done());

            /* pick out the shard result, and prepare to read it */
            intrusive_ptr<DocumentSourceBsonArray> pShardSource;
            BSONObjIterator shardIter(shardResult);
            while(shardIter.more()) {
                BSONElement shardElement(shardIter.next());
                const char *pFieldName = shardElement.fieldName();

                if ((strcmp(pFieldName, "result") == 0) ||
                    (strcmp(pFieldName, "serverPipeline") == 0)) {
                    pPipeline->addInitialSource(DocumentSourceBsonArray::create(&shardElement, pCtx));
                    pPipeline->stitch();

                    /*
                    Connect the output of the shard pipeline with the mongos
                    pipeline that will merge the results.
                    */
                    pPipeline->run(result);
                    return true;
                }
            }

            /* NOTREACHED */
            verify(false);
            return false;
        }
    } cmdPipeline;

} // namespace mongo
