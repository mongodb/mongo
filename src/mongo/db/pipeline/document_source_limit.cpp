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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceLimit::limitName[] = "$limit";

    DocumentSourceLimit::DocumentSourceLimit(const intrusive_ptr<ExpressionContext> &pExpCtx,
                                             long long limit)
        : SplittableDocumentSource(pExpCtx)
        , limit(limit)
        , count(0)
    {}

    const char *DocumentSourceLimit::getSourceName() const {
        return limitName;
    }

    bool DocumentSourceLimit::coalesce(
        const intrusive_ptr<DocumentSource> &pNextSource) {
        DocumentSourceLimit *pLimit =
            dynamic_cast<DocumentSourceLimit *>(pNextSource.get());

        /* if it's not another $skip, we can't coalesce */
        if (!pLimit)
            return false;

        /* we need to limit by the minimum of the two limits */
        if (pLimit->limit < limit)
            limit = pLimit->limit;
        return true;
    }

    boost::optional<Document> DocumentSourceLimit::getNext() {
        pExpCtx->checkForInterrupt();

        if (++count > limit) {
            pSource->dispose();
            return boost::none;
        }

        return pSource->getNext();
    }

    Value DocumentSourceLimit::serialize(bool explain) const {
        return Value(DOC(getSourceName() << limit));
    }

    intrusive_ptr<DocumentSourceLimit> DocumentSourceLimit::create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            long long limit) {
        uassert(15958, "the limit must be positive",
                limit > 0);
        return new DocumentSourceLimit(pExpCtx, limit);
    }

    intrusive_ptr<DocumentSource> DocumentSourceLimit::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15957, "the limit must be specified as a number",
                pBsonElement->isNumber());

        long long limit = pBsonElement->numberLong();
        return DocumentSourceLimit::create(pExpCtx, limit);
    }
}
