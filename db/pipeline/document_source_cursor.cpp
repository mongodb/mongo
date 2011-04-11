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

#include "db/Cursor.h"
#include "db/pipeline/document.h"

namespace mongo {

    DocumentSourceCursor::DocumentSourceCursor(
	boost::shared_ptr<Cursor> pTheCursor):
        pCursor(pTheCursor) {
    }

    boost::shared_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
	boost::shared_ptr<Cursor> pCursor) {
	boost::shared_ptr<DocumentSourceCursor> pSource(
	    new DocumentSourceCursor(pCursor));
	    return pSource;
    }

    bool DocumentSourceCursor::eof() {
        return pCursor->eof();
    }

    bool DocumentSourceCursor::advance() {
        return pCursor->advance();
    }

    boost::shared_ptr<Document> DocumentSourceCursor::getCurrent() {
        BSONObj documentObj(pCursor->current());
        boost::shared_ptr<Document> pDocument(
            Document::createFromBsonObj(&documentObj));
        return pDocument;
    }

    void DocumentSourceCursor::setSource(
	boost::shared_ptr<DocumentSource> pSource) {
	/* this doesn't take a source */
	assert(false);
    }

    DocumentSourceCursor::~DocumentSourceCursor() {
    }

    void DocumentSourceCursor::sourceToBson(BSONObjBuilder *pBuilder) const {
	assert(false); // this has no analog in the BSON world
    }
}
