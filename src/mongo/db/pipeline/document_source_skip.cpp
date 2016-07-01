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

DocumentSourceSkip::DocumentSourceSkip(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _skip(0), _needToSkip(true) {}

REGISTER_DOCUMENT_SOURCE(skip, DocumentSourceSkip::createFromBson);

const char* DocumentSourceSkip::getSourceName() const {
    return "$skip";
}

boost::optional<Document> DocumentSourceSkip::getNext() {
    pExpCtx->checkForInterrupt();

    if (_needToSkip) {
        _needToSkip = false;
        for (long long i = 0; i < _skip; i++) {
            if (!pSource->getNext())
                return boost::none;
        }
    }

    return pSource->getNext();
}

Value DocumentSourceSkip::serialize(bool explain) const {
    return Value(DOC(getSourceName() << _skip));
}

intrusive_ptr<DocumentSource> DocumentSourceSkip::optimize() {
    return _skip == 0 ? nullptr : this;
}

Pipeline::SourceContainer::iterator DocumentSourceSkip::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());
    auto nextSkip = dynamic_cast<DocumentSourceSkip*>((*std::next(itr)).get());

    if (nextLimit) {
        // Swap the $limit before this stage, allowing a top-k sort to be possible, provided there
        // is a $sort stage.
        nextLimit->setLimit(nextLimit->getLimit() + _skip);
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    } else if (nextSkip) {
        _skip += nextSkip->getSkip();
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

intrusive_ptr<DocumentSourceSkip> DocumentSourceSkip::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx) {
    intrusive_ptr<DocumentSourceSkip> pSource(new DocumentSourceSkip(pExpCtx));
    pSource->injectExpressionContext(pExpCtx);
    return pSource;
}

intrusive_ptr<DocumentSource> DocumentSourceSkip::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15972,
            str::stream() << "Argument to $skip must be a number not a " << typeName(elem.type()),
            elem.isNumber());

    intrusive_ptr<DocumentSourceSkip> pSkip(DocumentSourceSkip::create(pExpCtx));

    pSkip->_skip = elem.numberLong();
    uassert(15956, "Argument to $skip cannot be negative", pSkip->_skip >= 0);

    return pSkip;
}
}
