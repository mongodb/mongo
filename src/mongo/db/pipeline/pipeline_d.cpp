/**
 * Copyright (c) 2012-2014 MongoDB Inc.
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

#include "mongo/db/pipeline/pipeline_d.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/s/d_logic.h"

namespace mongo {

namespace {
    class MongodImplementation : public DocumentSourceNeedsMongod::MongodInterface {
    public:
        MongodImplementation(const intrusive_ptr<ExpressionContext>& ctx)
            : _ctx(ctx)
            , _client(ctx->opCtx)
        {}

        DBClientBase* directClient() {
            // opCtx may have changed since our last call
            invariant(_ctx->opCtx);
            _client.setOpCtx(_ctx->opCtx);
            return &_client;
        }

        bool isSharded(const NamespaceString& ns) {
            const ChunkVersion unsharded(0, 0, OID());
            return !(shardingState.getVersion(ns.ns()).isWriteCompatibleWith(unsharded));
        }

        bool isCapped(const NamespaceString& ns) {
            Client::ReadContext ctx(_ctx->opCtx, ns.ns());
            Collection* collection = ctx.ctx().db()->getCollection(_ctx->opCtx, ns);
            return collection && collection->isCapped();
        }

    private:
        intrusive_ptr<ExpressionContext> _ctx;
        DBDirectClient _client;
    };
}

    boost::shared_ptr<PlanExecutor> PipelineD::prepareCursorSource(
            OperationContext* txn,
            Collection* collection,
            const intrusive_ptr<Pipeline>& pPipeline,
            const intrusive_ptr<ExpressionContext>& pExpCtx) {
        // get the full "namespace" name
        const string& fullName = pExpCtx->ns.ns();
        pExpCtx->opCtx->lockState()->assertAtLeastReadLocked(fullName);

        // We will be modifying the source vector as we go
        Pipeline::SourceContainer& sources = pPipeline->sources;

        // Inject a MongodImplementation to sources that need them.
        for (size_t i = 0; i < sources.size(); i++) {
            DocumentSourceNeedsMongod* needsMongod =
                dynamic_cast<DocumentSourceNeedsMongod*>(sources[i].get());
            if (needsMongod) {
                needsMongod->injectMongodInterface(
                    boost::make_shared<MongodImplementation>(pExpCtx));
            }
        }

        if (!sources.empty() && sources.front()->isValidInitialSource()) {
            if (dynamic_cast<DocumentSourceMergeCursors*>(sources.front().get())) {
                // Enable the hooks for setting up authentication on the subsequent internal
                // connections we are going to create. This would normally have been done
                // when SetShardVersion was called, but since SetShardVersion is never called
                // on secondaries, this is needed.
                ShardedConnectionInfo::addHook();
            }
            return boost::shared_ptr<PlanExecutor>(); // don't need a cursor
        }


        // Look for an initial match. This works whether we got an initial query or not.
        // If not, it results in a "{}" query, which will be what we want in that case.
        const BSONObj queryObj = pPipeline->getInitialQuery();
        if (!queryObj.isEmpty()) {
            // This will get built in to the Cursor we'll create, so
            // remove the match from the pipeline
            sources.pop_front();
        }

        // Find the set of fields in the source documents depended on by this pipeline.
        const DepsTracker deps = pPipeline->getDependencies(queryObj);

        // Passing query an empty projection since it is faster to use ParsedDeps::extractFields().
        // This will need to change to support covering indexes (SERVER-12015). There is an
        // exception for textScore since that can only be retrieved by a query projection.
        const BSONObj projectionForQuery = deps.needTextScore ? deps.toProjection() : BSONObj();

        /*
          Look for an initial sort; we'll try to add this to the
          Cursor we create.  If we're successful in doing that (further down),
          we'll remove the $sort from the pipeline, because the documents
          will already come sorted in the specified order as a result of the
          index scan.
        */
        intrusive_ptr<DocumentSourceSort> sortStage;
        BSONObj sortObj;
        if (!sources.empty()) {
            sortStage = dynamic_cast<DocumentSourceSort*>(sources.front().get());
            if (sortStage) {
                // build the sort key
                sortObj = sortStage->serializeSortKey(/*explain*/false).toBson();
            }
        }

        // Create the PlanExecutor.
        //
        // If we try to create a PlanExecutor that includes both the match and the
        // sort, and the two are incompatible wrt the available indexes, then
        // we don't get a PlanExecutor back.
        //
        // So we try to use both first.  If that fails, try again, without the
        // sort.
        //
        // If we don't have a sort, jump straight to just creating a PlanExecutor.
        // without the sort.
        //
        // If we are able to incorporate the sort into the PlanExecutor, remove it
        // from the head of the pipeline.
        //
        // LATER - we should be able to find this out before we create the
        // cursor.  Either way, we can then apply other optimizations there
        // are tickets for, such as SERVER-4507.
        const size_t runnerOptions = QueryPlannerParams::DEFAULT
                                   | QueryPlannerParams::INCLUDE_SHARD_FILTER
                                   | QueryPlannerParams::NO_BLOCKING_SORT
                                   ;
        boost::shared_ptr<PlanExecutor> exec;
        bool sortInRunner = false;

        const WhereCallbackReal whereCallback(pExpCtx->opCtx, pExpCtx->ns.db());

        if (sortStage) {
            CanonicalQuery* cq;
            Status status =
                CanonicalQuery::canonicalize(pExpCtx->ns,
                                             queryObj,
                                             sortObj,
                                             projectionForQuery,
                                             &cq,
                                             whereCallback);
            PlanExecutor* rawExec;
            if (status.isOK() && getExecutor(txn, collection, cq, &rawExec, runnerOptions).isOK()) {
                // success: The PlanExecutor will handle sorting for us using an index.
                exec.reset(rawExec);
                sortInRunner = true;

                sources.pop_front();
                if (sortStage->getLimitSrc()) {
                    // need to reinsert coalesced $limit after removing $sort
                    sources.push_front(sortStage->getLimitSrc());
                }
            }
        }

        if (!exec.get()) {
            const BSONObj noSort;
            CanonicalQuery* cq;
            uassertStatusOK(
                CanonicalQuery::canonicalize(pExpCtx->ns,
                                             queryObj,
                                             noSort,
                                             projectionForQuery,
                                             &cq,
                                             whereCallback));

            PlanExecutor* rawExec;
            uassertStatusOK(getExecutor(txn, collection, cq, &rawExec, runnerOptions));
            exec.reset(rawExec);
        }


        // DocumentSourceCursor expects a yielding PlanExecutor that has had its state saved.
        exec->saveState();

        // Put the PlanExecutor into a DocumentSourceCursor and add it to the front of the pipeline.
        intrusive_ptr<DocumentSourceCursor> pSource =
            DocumentSourceCursor::create(fullName, exec, pExpCtx);

        // Note the query, sort, and projection for explain.
        pSource->setQuery(queryObj);
        if (sortInRunner)
            pSource->setSort(sortObj);

        pSource->setProjection(deps.toProjection(), deps.toParsedDeps());

        while (!sources.empty() && pSource->coalesce(sources.front())) {
            sources.pop_front();
        }

        pPipeline->addInitialSource(pSource);

        return exec;
    }

} // namespace mongo
