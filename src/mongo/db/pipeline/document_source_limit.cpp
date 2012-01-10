/**
*    Copyright (C) 2011 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceLimit::limitName[] = "$limit";

    DocumentSourceLimit::DocumentSourceLimit(const intrusive_ptr<ExpressionContext> &pTheCtx):
        limit(0),
        count(0),
        pCtx(pTheCtx) {
    }

    DocumentSourceLimit::~DocumentSourceLimit() {
    }

    bool DocumentSourceLimit::eof() {
        return pSource->eof() || count >= limit;
    }

    bool DocumentSourceLimit::advance() {
        ++count;
        if (count >= limit) {
            pCurrent.reset();
            return false;
        }
        pCurrent = pSource->getCurrent();
        return pSource->advance();
    }

    intrusive_ptr<Document> DocumentSourceLimit::getCurrent() {
        return pSource->getCurrent();
    }

    void DocumentSourceLimit::sourceToBson(BSONObjBuilder *pBuilder) const {
        pBuilder->append("$limit", limit);
    }

    intrusive_ptr<DocumentSourceLimit> DocumentSourceLimit::create(
        const intrusive_ptr<ExpressionContext> &pCtx) {
        intrusive_ptr<DocumentSourceLimit> pSource(
            new DocumentSourceLimit(pCtx));
        return pSource;
    }

    intrusive_ptr<DocumentSource> DocumentSourceLimit::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pCtx) {
        uassert(15957, "the limit must be specified as a number",
                pBsonElement->isNumber());

        intrusive_ptr<DocumentSourceLimit> pLimit(
            DocumentSourceLimit::create(pCtx));

        pLimit->limit = (int)pBsonElement->numberLong();
        uassert(15958, "the limit must be positive",
                pLimit->limit > 0);

        return pLimit;
    }
}
