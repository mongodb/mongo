/**
 * Copyright (c) 2012 10gen Inc.
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
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/s/d_logic.h"


namespace mongo {

namespace {
    class MongodImplementation : public DocumentSourceNeedsMongod::MongodInterface {
    public:
        DBClientBase* directClient() { return &_client; }

        bool isSharded(const NamespaceString& ns) {
            const ChunkVersion unsharded(0, 0, OID());
            return !(shardingState.getVersion(ns.ns()).isWriteCompatibleWith(unsharded));
        }

        bool isCapped(const NamespaceString& ns) {
            Client::ReadContext ctx(ns.ns());
            NamespaceDetails* nsd = nsdetails(ns.ns());
            return nsd && nsd->isCapped();
        }

    private:
        DBDirectClient _client;
    };
}

    void PipelineD::prepareCursorSource(
        const intrusive_ptr<Pipeline> &pPipeline,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {

        // We will be modifying the source vector as we go
        Pipeline::SourceContainer& sources = pPipeline->sources;

        // Inject a MongodImplementation to sources that need them.
        for (size_t i = 0; i < sources.size(); i++) {
            DocumentSourceNeedsMongod* needsMongod =
                dynamic_cast<DocumentSourceNeedsMongod*>(sources[i].get());
            if (needsMongod) {
                needsMongod->injectMongodInterface(boost::make_shared<MongodImplementation>());
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
            return; // don't need a cursor
        }


        // Look for an initial match. This works whether we got an initial query or not.
        // If not, it results in a "{}" query, which will be what we want in that case.
        const BSONObj queryObj = pPipeline->getInitialQuery();
        if (!queryObj.isEmpty()) {
            // This will get built in to the Cursor we'll create, so
            // remove the match from the pipeline
            sources.pop_front();
        }


        /* Look for an initial simple project; we'll avoid constructing Values
         * for fields that won't make it through the projection.
         */

        bool haveProjection = false;
        BSONObj projection;
        DocumentSource::ParsedDeps dependencies;
        {
            set<string> deps;
            DocumentSource::GetDepsReturn status = DocumentSource::SEE_NEXT;
            for (size_t i=0; i < sources.size() && status == DocumentSource::SEE_NEXT; i++) {
                status = sources[i]->getDependencies(deps);
                if (deps.count(string())) // empty string means we need the full doc
                    status = DocumentSource::NOT_SUPPORTED;
            }

            if (status == DocumentSource::EXHAUSTIVE) {
                projection = DocumentSource::depsToProjection(deps);
                dependencies = DocumentSource::parseDeps(deps);
                haveProjection = true;
            }
        }

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
                sortObj = sortStage->serializeSortKey().toBson();
            }
        }

        // get the full "namespace" name
        const string& fullName = pExpCtx->ns.ns();

        // for debugging purposes, show what the query and sort are
        DEV {
            (log() << "\n---- query BSON\n" <<
             queryObj.jsonString(Strict, 1) << "\n----\n");
            (log() << "\n---- sort BSON\n" <<
             sortObj.jsonString(Strict, 1) << "\n----\n");
            (log() << "\n---- fullName\n" <<
             fullName << "\n----\n");
        }

        // Create the necessary context to use a Runner, including taking a namespace read lock.
        // Note: this may throw if the sharding version for this connection is out of date.
        Client::ReadContext context(fullName);

        // Create the Runner.
        //
        // If we try to create a Runner that includes both the match and the
        // sort, and the two are incompatible wrt the available indexes, then
        // we don't get a Runner back.
        //
        // So we try to use both first.  If that fails, try again, without the
        // sort.
        //
        // If we don't have a sort, jump straight to just creating a Runner
        // without the sort.
        //
        // If we are able to incorporate the sort into the Runner, remove it
        // from the head of the pipeline.
        //
        // LATER - we should be able to find this out before we create the
        // cursor.  Either way, we can then apply other optimizations there
        // are tickets for, such as SERVER-4507.
        const size_t runnerOptions = QueryPlannerParams::DEFAULT
                                   | QueryPlannerParams::INCLUDE_COLLSCAN
                                   | QueryPlannerParams::INCLUDE_SHARD_FILTER
                                   | QueryPlannerParams::NO_BLOCKING_SORT
                                   ;
        auto_ptr<Runner> runner;
        bool sortInRunner = false;
        if (sortStage) {
            CanonicalQuery* cq;
            uassertStatusOK(CanonicalQuery::canonicalize(pExpCtx->ns,
                                                         queryObj,
                                                         sortObj,
                                                         projection,
                                                         &cq));
            Runner* rawRunner;
            if (getRunner(cq, &rawRunner, runnerOptions).isOK()) {
                // success: The Runner will handle sorting for us using an index.
                runner.reset(rawRunner);
                sortInRunner = true;

                sources.pop_front();
                if (sortStage->getLimitSrc()) {
                    // need to reinsert coalesced $limit after removing $sort
                    sources.push_front(sortStage->getLimitSrc());
                }
            }
        }

        if (!runner.get()) {
            const BSONObj noSort;
            CanonicalQuery* cq;
            uassertStatusOK(CanonicalQuery::canonicalize(pExpCtx->ns,
                                                         queryObj,
                                                         noSort,
                                                         projection,
                                                         &cq));

            Runner* rawRunner;
            uassertStatusOK(getRunner(cq, &rawRunner, runnerOptions));
            runner.reset(rawRunner);
        }

        // Now wrap the Runner in ClientCursor
        auto_ptr<ClientCursor> cursor(
            new ClientCursor(runner.release(), QueryOption_NoCursorTimeout));
        verify(cursor->getRunner());
        CursorId cursorId = cursor->cursorid();

        // Prepare the cursor for data to change under it when we unlock
        cursor->getRunner()->setYieldPolicy(Runner::YIELD_AUTO);
        cursor->getRunner()->saveState();
        cursor.release(); // it is now owned by the client cursor manager

        /* wrap the cursor with a DocumentSource and return that */
        intrusive_ptr<DocumentSourceCursor> pSource(
            DocumentSourceCursor::create( fullName, cursorId, pExpCtx ) );

        // Note the query, sort, and projection for explain.
        pSource->setQuery(queryObj);
        if (sortInRunner)
            pSource->setSort(sortObj);

        if (haveProjection) {
            pSource->setProjection(projection, dependencies);
        }

        while (!sources.empty() && pSource->coalesce(sources.front())) {
            sources.pop_front();
        }

        pPipeline->addInitialSource(pSource);
    }

} // namespace mongo
