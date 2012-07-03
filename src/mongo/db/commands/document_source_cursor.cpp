/**
 * Copyright 2011 (c) 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cursor.h"
#include "mongo/db/instance.h"
#include "mongo/db/pipeline/document.h"

namespace mongo {

    DocumentSourceCursor::~DocumentSourceCursor() {
    }

    void DocumentSourceCursor::releaseCursor() {
        // note the order here; the cursor holder has to go first
        pClientCursor.reset();
        pCursor.reset();
    }

    bool DocumentSourceCursor::eof() {
        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        return (pCurrent.get() == NULL);
    }

    bool DocumentSourceCursor::advance() {
        DocumentSource::advance(); // check for interrupts

        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        findNext();
        return (pCurrent.get() != NULL);
    }

    intrusive_ptr<Document> DocumentSourceCursor::getCurrent() {
        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        return pCurrent;
    }

    void DocumentSourceCursor::advanceAndYield() {
        pCursor->advance();
        /*
          TODO ask for index key pattern in order to determine which index
          was used for this particular document; that will allow us to
          sometimes use ClientCursor::MaybeCovered.
          See https://jira.mongodb.org/browse/SERVER-5224 .
        */
        bool cursorOk = pClientCursor->yieldSometimes(ClientCursor::WillNeed);
        if (!cursorOk) {
            uassert(16028,
                    "collection or database disappeared when cursor yielded",
                    false);
        }
    }

    void DocumentSourceCursor::findNext() {
        /* standard cursor usage pattern */
        while(pCursor->ok()) {
            CoveredIndexMatcher *pCIM; // save intermediate result
            if ((!(pCIM = pCursor->matcher()) ||
                 pCIM->matchesCurrent(pCursor.get())) &&
                !pCursor->getsetdup(pCursor->currLoc())) {

                /* grab the matching document */
                BSONObj documentObj(pCursor->current());
                pCurrent = Document::createFromBsonObj(
                    &documentObj, NULL /* LATER pDependencies.get()*/);
                advanceAndYield();
                return;
            }

            advanceAndYield();
        }

        /* if we got here, there aren't any more documents */
        pCurrent.reset();
    }

    void DocumentSourceCursor::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    void DocumentSourceCursor::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {

        /* this has no analog in the BSON world, so only allow it for explain */
        if (explain)
        {
            BSONObj bsonObj;
            
            pBuilder->append("query", *pQuery);

            if (pSort.get())
            {
                pBuilder->append("sort", *pSort);
            }

            // construct query for explain
            BSONObjBuilder queryBuilder;
            queryBuilder.append("$query", *pQuery);
            if (pSort.get())
                queryBuilder.append("$orderby", *pSort);
            queryBuilder.append("$explain", 1);
            Query query(queryBuilder.obj());

            DBDirectClient directClient;
            BSONObj explainResult(directClient.findOne(ns, query));

            pBuilder->append("cursor", explainResult);
        }
    }

    DocumentSourceCursor::DocumentSourceCursor(
        const shared_ptr<Cursor> &pTheCursor,
        const string &ns,
        const intrusive_ptr<ExpressionContext> &pCtx):
        DocumentSource(pCtx),
        pCurrent(),
        pCursor(pTheCursor),
        pClientCursor(),
        pDependencies() {
        pClientCursor.reset(
            new ClientCursor(QueryOption_NoCursorTimeout, pTheCursor, ns));
    }

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
        const shared_ptr<Cursor> &pCursor,
        const string &ns,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        verify(pCursor.get());
        intrusive_ptr<DocumentSourceCursor> pSource(
            new DocumentSourceCursor(pCursor, ns, pExpCtx));
            return pSource;
    }

    void DocumentSourceCursor::setNamespace(const string &n) {
        ns = n;
    }

    void DocumentSourceCursor::setQuery(const shared_ptr<BSONObj> &pBsonObj) {
        pQuery = pBsonObj;
    }

    void DocumentSourceCursor::setSort(const shared_ptr<BSONObj> &pBsonObj) {
        pSort = pBsonObj;
    }

    void DocumentSourceCursor::manageDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker) {
        /* hang on to the tracker */
        pDependencies = pTracker;
    }

}
