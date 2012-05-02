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

#include "db/commands/pipeline.h"
#include "db/commands/pipeline_d.h"
#include "db/cursor.h"
#include "db/interrupt_status_mongod.h"
#include "db/pdfile.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/document_source.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"

namespace mongo {

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class PipelineCommand :
        public Command {
    public:
        // virtuals from Command
        virtual ~PipelineCommand();
        virtual bool run(const string &db, BSONObj &cmdObj, int options,
                         string &errmsg, BSONObjBuilder &result, bool fromRepl);
        virtual LockType locktype() const;
        virtual bool slaveOk() const;
        virtual void help(stringstream &help) const;

        PipelineCommand();
    };

    // self-registering singleton static instance
    static PipelineCommand pipelineCommand;

    PipelineCommand::PipelineCommand():
        Command(Pipeline::commandName) {
    }

    Command::LockType PipelineCommand::locktype() const {
        return READ;
    }

    bool PipelineCommand::slaveOk() const {
        return true;
    }

    void PipelineCommand::help(stringstream &help) const {
        help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    PipelineCommand::~PipelineCommand() {
    }

    bool PipelineCommand::run(const string &db, BSONObj &cmdObj,
                              int options, string &errmsg,
                              BSONObjBuilder &result, bool fromRepl) {

        intrusive_ptr<ExpressionContext> pCtx(
            ExpressionContext::create(&InterruptStatusMongod::status));

        /* try to parse the command; if this fails, then we didn't run */
        intrusive_ptr<Pipeline> pPipeline(
            Pipeline::parseCommand(errmsg, cmdObj, pCtx));
        if (!pPipeline.get())
            return false;

        intrusive_ptr<DocumentSource> pSource(
            PipelineD::prepareCursorSource(pPipeline, db, pCtx));

        /* this is the normal non-debug path */
        if (!pPipeline->getSplitMongodPipeline())
            return pPipeline->run(result, errmsg, pSource);

        /* setup as if we're in the router */
        pCtx->setInRouter(true);

        /*
          Here, we'll split the pipeline in the same way we would for sharding,
          for testing purposes.

          Run the shard pipeline first, then feed the results into the remains
          of the existing pipeline.

          Start by splitting the pipeline.
         */
        intrusive_ptr<Pipeline> pShardSplit(
            pPipeline->splitForSharded());

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

        /* run the shard pipeline */
        BSONObjBuilder shardResultBuilder;
        string shardErrmsg;
        pShardPipeline->run(shardResultBuilder, shardErrmsg, pSource);
        BSONObj shardResult(shardResultBuilder.done());

        /* pick out the shard result, and prepare to read it */
        intrusive_ptr<DocumentSourceBsonArray> pShardSource;
        BSONObjIterator shardIter(shardResult);
        while(shardIter.more()) {
            BSONElement shardElement(shardIter.next());
            const char *pFieldName = shardElement.fieldName();

            if (strcmp(pFieldName, "result") == 0) {
                pShardSource = DocumentSourceBsonArray::create(
                    &shardElement, pCtx);

                /*
                  Connect the output of the shard pipeline with the mongos
                  pipeline that will merge the results.
                */
                return pPipeline->run(result, errmsg, pShardSource);
            }
        }

        /* NOTREACHED */
        verify(false);
        return false;
    }

} // namespace mongo
