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
#include "mongo/db/cursor.h"
#include "mongo/db/instance.h"
#include "mongo/db/parsed_query.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query_optimizer.h"
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

    private:
        DBDirectClient _client;
    };

}

    void PipelineD::prepareCursorSource(
        const intrusive_ptr<Pipeline> &pPipeline,
        const string &dbName,
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

        /* look for an initial match */
        BSONObjBuilder queryBuilder;
        bool initQuery = pPipeline->getInitialQuery(&queryBuilder);
        if (initQuery) {
            /*
              This will get built in to the Cursor we'll create, so
              remove the match from the pipeline
            */
            sources.pop_front();
        }

        /*
          Create a query object.

          This works whether we got an initial query above or not; if not,
          it results in a "{}" query, which will be what we want in that case.

          We create a pointer to a shared object instead of a local
          object so that we can preserve it for the Cursor we're going to
          create below.
         */
        BSONObj queryObj = queryBuilder.obj();

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
        intrusive_ptr<DocumentSourceSort> pSort;
        BSONObj sortObj;
        if (!sources.empty()) {
            const intrusive_ptr<DocumentSource> &pSC = sources.front();
            pSort = dynamic_cast<DocumentSourceSort *>(pSC.get());

            if (pSort) {
                // build the sort key
                sortObj = pSort->serializeSortKey().toBson();
            }
        }

        // get the full "namespace" name
        string fullName(dbName + "." + pPipeline->getCollectionName());

        // for debugging purposes, show what the query and sort are
        DEV {
            (log() << "\n---- query BSON\n" <<
             queryObj.jsonString(Strict, 1) << "\n----\n");
            (log() << "\n---- sort BSON\n" <<
             sortObj.jsonString(Strict, 1) << "\n----\n");
            (log() << "\n---- fullName\n" <<
             fullName << "\n----\n");
        }

        // Create the necessary context to use a Cursor, including taking a namespace read lock,
        // see SERVER-6123.
        // Note: this may throw if the sharding version for this connection is out of date.
        Client::ReadContext context(fullName);

        /*
          Create the cursor.

          If we try to create a cursor that includes both the match and the
          sort, and the two are incompatible wrt the available indexes, then
          we don't get a cursor back.

          So we try to use both first.  If that fails, try again, without the
          sort.

          If we don't have a sort, jump straight to just creating a cursor
          without the sort.

          If we are able to incorporate the sort into the cursor, remove it
          from the head of the pipeline.

          LATER - we should be able to find this out before we create the
          cursor.  Either way, we can then apply other optimizations there
          are tickets for, such as SERVER-4507.
         */

        shared_ptr<Cursor> pCursor;
        bool initSort = false;
        if (pSort) {
            const BSONObj queryAndSort = BSON("$query" << queryObj << "$orderby" << sortObj);
            shared_ptr<ParsedQuery> pq (new ParsedQuery(
                fullName.c_str(), 0, 0, QueryOption_NoCursorTimeout, queryAndSort, projection));

            /* try to create the cursor with the query and the sort */
            shared_ptr<Cursor> pSortedCursor(
                getOptimizedCursor(
                    fullName.c_str(), queryObj, sortObj,
                    QueryPlanSelectionPolicy::any(), pq));

            if (pSortedCursor.get()) {
                /* success:  remove the sort from the pipeline */
                sources.pop_front();

                if (pSort->getLimitSrc()) {
                    // need to reinsert coalesced $limit after removing $sort
                    sources.push_front(pSort->getLimitSrc());
                }

                pCursor = pSortedCursor;
                initSort = true;
            }
        }

        if (!pCursor.get()) {
            shared_ptr<ParsedQuery> pq (new ParsedQuery(
                fullName.c_str(), 0, 0, QueryOption_NoCursorTimeout, queryObj, projection));

            /* try to create the cursor without the sort */
            shared_ptr<Cursor> pUnsortedCursor(
                getOptimizedCursor(
                    fullName.c_str(), queryObj, BSONObj(),
                    QueryPlanSelectionPolicy::any(), pq));

            pCursor = pUnsortedCursor;
        }

        // Now wrap the Cursor in ClientCursor
        ClientCursorHolder cursor(
                new ClientCursor(QueryOption_NoCursorTimeout, pCursor, fullName));
        CursorId cursorId = cursor->cursorid();
        massert(16917, str::stream()
                            << "cursor " << cursor->c()->toString()
                            << "does its own locking so it can't be used with aggregation",
                cursor->c()->requiresLock());

        // Prepare the cursor for data to change under it when we unlock
        if (cursor->c()->supportYields()) {
            ClientCursor::YieldData data;
            cursor->prepareToYield(data);
        }
        else {
            massert(16915, str::stream()
                                << "cursor " << cursor->c()->toString()
                                << " supports neither yields nor getMore, one of which"
                                << " must be supported in an aggregation source",
                    cursor->c()->supportGetMore());

            cursor->c()->noteLocation();
        }
        cursor.release(); // it is now owned by the client cursor manager

        /* wrap the cursor with a DocumentSource and return that */
        intrusive_ptr<DocumentSourceCursor> pSource(
            DocumentSourceCursor::create( fullName, cursorId, pExpCtx ) );

        /*
          Note the query and sort

          This records them for explain, and keeps them alive; they are
          referenced (by reference) by the cursor, which doesn't make its
          own copies of them.
        */
        pSource->setQuery(queryObj);
        if (initSort)
            pSource->setSort(sortObj);

        if (haveProjection) {
            pSource->setProjection(projection, dependencies);
        }

        while (!sources.empty() && pSource->coalesce(sources.front())) {
            sources.pop_front();
        }

        // If we are in an explain, we won't actually use the created cursor so release it.
        if (pPipeline->isExplain())
            pSource->dispose();

        pPipeline->addInitialSource(pSource);
    }

} // namespace mongo
