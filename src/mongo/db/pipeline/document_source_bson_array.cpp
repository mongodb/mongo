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

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"


namespace mongo {

    DocumentSourceBsonArray::~DocumentSourceBsonArray() {
    }

    bool DocumentSourceBsonArray::eof() {
        return !haveCurrent;
    }

    bool DocumentSourceBsonArray::advance() {
        DocumentSource::advance(); // check for interrupts

        if (eof())
            return false;

        if (!arrayIterator.more()) {
            haveCurrent = false;
            return false;
        }

        currentElement = arrayIterator.next();
        return true;
    }

    Document DocumentSourceBsonArray::getCurrent() {
        verify(haveCurrent);
        BSONObj documentObj(currentElement.Obj());
        Document pDocument(
            Document::createFromBsonObj(&documentObj));
        return pDocument;
    }

    void DocumentSourceBsonArray::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    DocumentSourceBsonArray::DocumentSourceBsonArray(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        embeddedObject(pBsonElement->embeddedObject()),
        arrayIterator(embeddedObject),
        haveCurrent(false) {
        if (arrayIterator.more()) {
            currentElement = arrayIterator.next();
            haveCurrent = true;
        }
    }

    intrusive_ptr<DocumentSourceBsonArray> DocumentSourceBsonArray::create(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {

        verify(pBsonElement->type() == Array);
        intrusive_ptr<DocumentSourceBsonArray> pSource(
            new DocumentSourceBsonArray(pBsonElement, pExpCtx));

        return pSource;
    }

    void DocumentSourceBsonArray::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {

        if (explain) {
            BSONObj empty;

            pBuilder->append("bsonArray", empty);
        }
    }
}
