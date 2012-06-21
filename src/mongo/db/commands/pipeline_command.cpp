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

    private:
        /*
          For the case of explain, we don't want to hold any lock at all,
          because it generates warnings about recursive locks.  However,
          the getting the explain information for the underlying cursor uses
          the direct client cursor, and that gets a lock.  Therefore, we need
          to take steps to avoid holding a lock while we use that.  On the
          other hand, we need to have a READ lock for normal explain execution.
          Therefore, the lock is managed manually, and not through the virtual
          locktype() above.

          In order to achieve this, locktype() returns NONE, but the lock that
          would be managed for reading (for executing the pipeline in the
          regular way),  will be managed manually here.  This code came from
          dbcommands.cpp, where objects are constructed to hold the lock
          and automatically release it on destruction.  The use of this
          pattern requires extra functions to hold the lock scope and from
          within which to execute the other steps of the explain.

          The arguments for these are all the same, and come from run(), but
          are passed in so that new blocks can be created to hold the
          automatic locking objects.
         */

        /*
          Execute the pipeline for the explain.  This is common to both the
          locked and unlocked code path.  However, the results are different.
          For an explain, with no lock, it really outputs the pipeline
          chain rather than fetching the data.
         */
        bool executePipeline(
            BSONObjBuilder &result, string &errmsg, const string &ns,
            intrusive_ptr<Pipeline> &pPipeline,
            intrusive_ptr<DocumentSourceCursor> &pSource,
            intrusive_ptr<ExpressionContext> &pCtx);

        /*
          The explain code path holds a lock while the original cursor is
          parsed; we still need to take that step, because that is how we
          determine whether or not indexes will allow the optimization of
          early $match and/or $sort.

          Once the Cursor is identified, it is released, and then the lock
          is released (automatically, via end of a block), and then the
          pipeline is executed.
         */
        bool runExplain(
            BSONObjBuilder &result, string &errmsg,
            const string &ns, const string &db,
            intrusive_ptr<Pipeline> &pPipeline,
            intrusive_ptr<ExpressionContext> &pCtx);

        /*
          The execute code path holds a READ lock for its entire duration.
          The Cursor is created, and then documents are pulled out of it until
          they are exhausted (or some other error occurs).
         */
        bool runExecute(
            BSONObjBuilder &result, string &errmsg,
            const string &ns, const string &db,
            intrusive_ptr<Pipeline> &pPipeline,
            intrusive_ptr<ExpressionContext> &pCtx);
    };

    // self-registering singleton static instance
    static PipelineCommand pipelineCommand;

    PipelineCommand::PipelineCommand():
        Command(Pipeline::commandName) {
    }

    Command::LockType PipelineCommand::locktype() const {
        /*
          The locks for this are managed manually.  The problem is that the
          explain execution uses the direct client interface, and this
          causes recursive lock warnings if the lock is already held.  As
          a result, there are two code paths for this.  See the comments in
          the private section of PipelineCommand for more details.
         */
        return NONE;
    }

    bool PipelineCommand::slaveOk() const {
        return true;
    }

    void PipelineCommand::help(stringstream &help) const {
        help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    PipelineCommand::~PipelineCommand() {
    }

    bool PipelineCommand::runExplain(
        BSONObjBuilder &result, string &errmsg,
        const string &ns, const string &db,
        intrusive_ptr<Pipeline> &pPipeline,
        intrusive_ptr<ExpressionContext> &pCtx) {

        intrusive_ptr<DocumentSourceCursor> pSource;
        
        /*
          For EXPLAIN:

          This block is here to contain the scope of the lock.  We need the lock
          while we prepare the cursor, but we need to have released it by the
          time the recursive call is made to get the explain information using
          the direct client interface under the execution phase.
         */
        {
            scoped_ptr<Lock::GlobalRead> lk;
            if(lockGlobally())
                lk.reset(new Lock::GlobalRead());
            Client::ReadContext ctx(ns, dbpath, requiresAuth()); // read lock

            pSource = PipelineD::prepareCursorSource(pPipeline, db, pCtx);

            /* release the Cursor before the lock gets released */
            pSource->releaseCursor();
        }

        /*
          For EXPLAIN this just uses the direct client to do an explain on
          what the underlying Cursor was, based on its query and sort
          settings, and then wraps it with JSON from the pipeline definition.
          That does not require the lock or cursor, both of which were
          released above.
         */
        return executePipeline(result, errmsg, ns, pPipeline, pSource, pCtx);
    }

    bool PipelineCommand::runExecute(
        BSONObjBuilder &result, string &errmsg,
        const string &ns, const string &db,
        intrusive_ptr<Pipeline> &pPipeline,
        intrusive_ptr<ExpressionContext> &pCtx) {

        scoped_ptr<Lock::GlobalRead> lk;
        if(lockGlobally())
            lk.reset(new Lock::GlobalRead());
        Client::ReadContext ctx(ns, dbpath, requiresAuth()); // read lock

        intrusive_ptr<DocumentSourceCursor> pSource(
            PipelineD::prepareCursorSource(pPipeline, db, pCtx));
        return executePipeline(result, errmsg, ns, pPipeline, pSource, pCtx);
    }

    bool PipelineCommand::executePipeline(
        BSONObjBuilder &result, string &errmsg, const string &ns,
        intrusive_ptr<Pipeline> &pPipeline,
        intrusive_ptr<DocumentSourceCursor> &pSource,
        intrusive_ptr<ExpressionContext> &pCtx) {

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

            if ((strcmp(pFieldName, "result") == 0) ||
                (strcmp(pFieldName, "serverPipeline") == 0)) {
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

        string ns(parseNs(db, cmdObj));

        if (pPipeline->isExplain())
            return runExplain(result, errmsg, ns, db, pPipeline, pCtx);
        else
            return runExecute(result, errmsg, ns, db, pPipeline, pCtx);
    }

} // namespace mongo
