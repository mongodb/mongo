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

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/cursor.h"
#include "db/pipeline/document.h"

namespace mongo {

    DocumentSourceCursor::~DocumentSourceCursor() {
    }

    bool DocumentSourceCursor::eof() {
        /* if we haven't gotten the first one yet, do so now */
        if (!pCurrent.get())
            findNext();

        return (pCurrent.get() == NULL);
    }

    bool DocumentSourceCursor::advance() {
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

    void DocumentSourceCursor::findNext() {
        /* standard cursor usage pattern */
        while(pCursor->ok()) {
            CoveredIndexMatcher *pCIM; // save intermediate result
            if ((!(pCIM = pCursor->matcher()) ||
                 pCIM->matchesCurrent(pCursor.get())) &&
                !pCursor->getsetdup(pCursor->currLoc())) {

                /* grab the matching document */
                BSONObj documentObj(pCursor->current());
                pCurrent = Document::createFromBsonObj(&documentObj);
                pCursor->advance();
                return;
            }

            pCursor->advance();
        }

        /* if we got here, there aren't any more documents */
        pCurrent.reset();
    }

    void DocumentSourceCursor::setSource(
        const intrusive_ptr<DocumentSource> &pSource) {
        /* this doesn't take a source */
        assert(false);
    }

    void DocumentSourceCursor::sourceToBson(BSONObjBuilder *pBuilder) const {
        /* this has no analog in the BSON world */
        assert(false);
    }

    DocumentSourceCursor::DocumentSourceCursor(
        const shared_ptr<Cursor> &pTheCursor):
        pCurrent(),
        bsonDependencies(),
        pCursor(pTheCursor) {
    }

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
        const shared_ptr<Cursor> &pCursor) {
        assert(pCursor.get());
        intrusive_ptr<DocumentSourceCursor> pSource(
            new DocumentSourceCursor(pCursor));
            return pSource;
    }

    void DocumentSourceCursor::addBsonDependency(
        const shared_ptr<BSONObj> &pBsonObj) {
        bsonDependencies.push_back(pBsonObj);
    }
}
