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
 */

#include "pch.h"

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

namespace mongo {

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
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }

        virtual bool run(const string &db, BSONObj &cmdObj, int options, string &errmsg,
                         BSONObjBuilder &result, bool fromRepl) {

            intrusive_ptr<ExpressionContext> pCtx =
                ExpressionContext::create(&InterruptStatusMongod::status);

            /* try to parse the command; if this fails, then we didn't run */
            intrusive_ptr<Pipeline> pPipeline = Pipeline::parseCommand(errmsg, cmdObj, pCtx);
            if (!pPipeline.get())
                return false;

            string ns = parseNs(db, cmdObj);

            if (pPipeline->getSplitMongodPipeline()) {
                // This is only used in testing
                return executeSplitPipeline(result, errmsg, ns, db, pPipeline, pCtx);
            }

#if _DEBUG
            // This is outside of the if block to keep the object alive until the pipeline is finished.
            BSONObj parsed;
            if (!pPipeline->isExplain() && !pCtx->getInShard()) {
                // Make sure all operations round-trip through Pipeline::toBson()
                // correctly by reparsing every command on DEBUG builds. This is
                // important because sharded aggregations rely on this ability.
                // Skipping when inShard because this has already been through the
                // transformation (and this unsets pCtx->inShard).
                BSONObjBuilder bb;
                pPipeline->toBson(&bb);
                parsed = bb.obj();
                pPipeline = Pipeline::parseCommand(errmsg, parsed, pCtx);
                verify(pPipeline);
            }
#endif

            // This does the mongod-specific stuff like creating a cursor
            PipelineD::prepareCursorSource(pPipeline, nsToDatabase(ns), pCtx);
            return pPipeline->run(result, errmsg);
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
            pCtx->setInRouter(true);

            /*
            Here, we'll split the pipeline in the same way we would for sharding,
            for testing purposes.

            Run the shard pipeline first, then feed the results into the remains
            of the existing pipeline.

            Start by splitting the pipeline.
            */
            intrusive_ptr<Pipeline> pShardSplit = pPipeline->splitForSharded();

            /*
            Write the split pipeline as we would in order to transmit it to
            the shard servers.
            */
            BSONObjBuilder shardBuilder;
            pShardSplit->toBson(&shardBuilder);
            BSONObj shardBson(shardBuilder.done());

            DEV (log() << "\n---- shardBson\n" <<
                shardBson.jsonString(Strict, 1) << "\n----\n").flush();

            /* for debugging purposes, show what the pipeline now looks like */
            DEV {
                BSONObjBuilder pipelineBuilder;
                pPipeline->toBson(&pipelineBuilder);
                BSONObj pipelineBson(pipelineBuilder.done());
                (log() << "\n---- pipelineBson\n" <<
                pipelineBson.jsonString(Strict, 1) << "\n----\n").flush();
            }

            /* on the shard servers, create the local pipeline */
            intrusive_ptr<ExpressionContext> pShardCtx(
                ExpressionContext::create(&InterruptStatusMongod::status));
            intrusive_ptr<Pipeline> pShardPipeline(
                Pipeline::parseCommand(errmsg, shardBson, pShardCtx));
            if (!pShardPipeline.get()) {
                return false;
            }

            PipelineD::prepareCursorSource(pShardPipeline, nsToDatabase(ns), pCtx);

            /* run the shard pipeline */
            BSONObjBuilder shardResultBuilder;
            string shardErrmsg;
            pShardPipeline->run(shardResultBuilder, shardErrmsg);
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

                    /*
                    Connect the output of the shard pipeline with the mongos
                    pipeline that will merge the results.
                    */
                    return pPipeline->run(result, errmsg);
                }
            }

            /* NOTREACHED */
            verify(false);
            return false;
        }
    } cmdPipeline;

} // namespace mongo
