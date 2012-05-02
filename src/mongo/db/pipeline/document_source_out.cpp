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


namespace mongo {

    const char DocumentSourceOut::outName[] = "$out";

    DocumentSourceOut::~DocumentSourceOut() {
    }

    const char *DocumentSourceOut::getSourceName() const {
        return outName;
    }

    bool DocumentSourceOut::eof() {
        return pSource->eof();
    }

    bool DocumentSourceOut::advance() {
        DocumentSource::advance(); // check for interrupts

        return pSource->advance();
    }

    boost::intrusive_ptr<Document> DocumentSourceOut::getCurrent() {
        return pSource->getCurrent();
    }

    DocumentSourceOut::DocumentSourceOut(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx) {
        verify(false && "unimplemented");
    }

    intrusive_ptr<DocumentSourceOut> DocumentSourceOut::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceOut> pSource(
            new DocumentSourceOut(pBsonElement, pExpCtx));

        return pSource;
    }

    void DocumentSourceOut::sourceToBson(BSONObjBuilder *pBuilder) const {
        verify(false); // CW TODO
    }
}
