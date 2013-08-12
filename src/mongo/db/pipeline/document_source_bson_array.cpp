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

    boost::optional<Document> DocumentSourceBsonArray::getNext() {
        pExpCtx->checkForInterrupt();

        if (!arrayIterator.more())
            return boost::none;

        return Document(arrayIterator.next().Obj());
    }

    void DocumentSourceBsonArray::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    DocumentSourceBsonArray::DocumentSourceBsonArray(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx)
        : DocumentSource(pExpCtx)
        , embeddedObject(pBsonElement->embeddedObject())
        , arrayIterator(embeddedObject)
    {}

    intrusive_ptr<DocumentSourceBsonArray> DocumentSourceBsonArray::create(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {

        verify(pBsonElement->type() == Array);
        intrusive_ptr<DocumentSourceBsonArray> pSource(
            new DocumentSourceBsonArray(pBsonElement, pExpCtx));

        return pSource;
    }

    Value DocumentSourceBsonArray::serialize(bool explain) const {
        if (explain) {
            return Value(DOC("bsonArray" << Document()));
        }
        return Value();
    }
}
