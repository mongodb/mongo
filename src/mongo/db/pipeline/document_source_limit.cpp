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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceLimit::DocumentSourceLimit(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                         long long limit)
    : DocumentSource(pExpCtx), limit(limit), count(0) {}

REGISTER_DOCUMENT_SOURCE(limit, DocumentSourceLimit::createFromBson);

const char* DocumentSourceLimit::getSourceName() const {
    return "$limit";
}

Pipeline::SourceContainer::iterator DocumentSourceLimit::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        limit = std::min(limit, nextLimit->limit);
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
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
    const intrusive_ptr<ExpressionContext>& pExpCtx, long long limit) {
    uassert(15958, "the limit must be positive", limit > 0);
    intrusive_ptr<DocumentSourceLimit> source(new DocumentSourceLimit(pExpCtx, limit));
    source->injectExpressionContext(pExpCtx);
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceLimit::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15957, "the limit must be specified as a number", elem.isNumber());

    long long limit = elem.numberLong();
    return DocumentSourceLimit::create(pExpCtx, limit);
}
}
