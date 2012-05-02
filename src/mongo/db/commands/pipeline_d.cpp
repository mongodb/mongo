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
 */

#include "pch.h"
#include "db/commands/pipeline.h"
#include "db/commands/pipeline_d.h"

#include "db/cursor.h"
#include "db/pipeline/document_source.h"


namespace mongo {

    intrusive_ptr<DocumentSource> PipelineD::prepareCursorSource(
        const intrusive_ptr<Pipeline> &pPipeline,
        const string &dbName,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {

        Pipeline::SourceVector *pSources = &pPipeline->sourceVector;

        /* look for an initial match */
        BSONObjBuilder queryBuilder;
        bool initQuery = pPipeline->getInitialQuery(&queryBuilder);
        if (initQuery) {
            /*
              This will get built in to the Cursor we'll create, so
              remove the match from the pipeline
            */
            pSources->erase(pSources->begin());
        }

        /*
          Create a query object.

          This works whether we got an initial query above or not; if not,
          it results in a "{}" query, which will be what we want in that case.

          We create a pointer to a shared object instead of a local
          object so that we can preserve it for the Cursor we're going to
          create below.  See DocumentSourceCursor::addBsonDependency().
         */
        shared_ptr<BSONObj> pQueryObj(new BSONObj(queryBuilder.obj()));

        /*
          Look for an initial sort; we'll try to add this to the
          Cursor we create.  If we're successful, then 
        */
        const DocumentSourceSort *pSort = NULL;
        BSONObjBuilder sortBuilder;
        if (pSources->size()) {
            const intrusive_ptr<DocumentSource> &pSC = pSources->front();
            pSort = dynamic_cast<DocumentSourceSort *>(pSC.get());

            if (pSort) {
                /* build the sort key */
                pSort->sortKeyToBson(&sortBuilder, false);
            }
        }

        /* Create the sort object; see comments on the query object above */
        shared_ptr<BSONObj> pSortObj(new BSONObj(sortBuilder.obj()));

        /* get the full "namespace" name */
        string fullName(dbName + "." + pPipeline->getCollectionName());

        /* for debugging purposes, show what the query and sort are */
        DEV {
            (log() << "\n---- query BSON\n" <<
             pQueryObj->jsonString(Strict, 1) << "\n----\n").flush();
            (log() << "\n---- sort BSON\n" <<
             pSortObj->jsonString(Strict, 1) << "\n----\n").flush();
            (log() << "\n---- fullName\n" <<
             fullName << "\n----\n").flush();
        }
        
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
            /* try to create the cursor with the query and the sort */
            shared_ptr<Cursor> pSortedCursor(
                pCursor = NamespaceDetailsTransient::getCursor(
                    fullName.c_str(), *pQueryObj, *pSortObj));

            if (pSortedCursor.get()) {
                /* success:  remove the sort from the pipeline */
                pSources->erase(pSources->begin());

                pCursor = pSortedCursor;
                initSort = true;
            }
        }

        if (!pCursor.get()) {
            /* try to create the cursor without the sort */
            shared_ptr<Cursor> pUnsortedCursor(
                pCursor = NamespaceDetailsTransient::getCursor(
                    fullName.c_str(), *pQueryObj));

            pCursor = pUnsortedCursor;
        }

        /* wrap the cursor with a DocumentSource and return that */
        intrusive_ptr<DocumentSourceCursor> pSource(
            DocumentSourceCursor::create(pCursor, dbName, pExpCtx));

        /* record any dependencies we created */
        if (initQuery)
            pSource->addBsonDependency(pQueryObj);
        if (initSort)
            pSource->addBsonDependency(pSortObj);

        return pSource;
    }

} // namespace mongo
