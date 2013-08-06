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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    const char DocumentSourceSkip::skipName[] = "$skip";

    DocumentSourceSkip::DocumentSourceSkip(const intrusive_ptr<ExpressionContext> &pExpCtx):
        SplittableDocumentSource(pExpCtx),
        _skip(0),
        _needToSkip(true) {
    }

    const char *DocumentSourceSkip::getSourceName() const {
        return skipName;
    }

    bool DocumentSourceSkip::coalesce(
        const intrusive_ptr<DocumentSource> &pNextSource) {
        DocumentSourceSkip *pSkip =
            dynamic_cast<DocumentSourceSkip *>(pNextSource.get());

        /* if it's not another $skip, we can't coalesce */
        if (!pSkip)
            return false;

        /* we need to skip over the sum of the two consecutive $skips */
        _skip += pSkip->_skip;
        return true;
    }

    boost::optional<Document> DocumentSourceSkip::getNext() {
        pExpCtx->checkForInterrupt();

        if (_needToSkip) {
            _needToSkip = false;
            for (long long i=0; i < _skip; i++) {
                if (!pSource->getNext())
                    return boost::none;
            }
        }

        return pSource->getNext();
    }

    void DocumentSourceSkip::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {
        pBuilder->append("$skip", _skip);
    }

    intrusive_ptr<DocumentSourceSkip> DocumentSourceSkip::create(
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceSkip> pSource(
            new DocumentSourceSkip(pExpCtx));
        return pSource;
    }

    intrusive_ptr<DocumentSource> DocumentSourceSkip::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15972, str::stream() << DocumentSourceSkip::skipName <<
                ":  the value to skip must be a number",
                pBsonElement->isNumber());

        intrusive_ptr<DocumentSourceSkip> pSkip(
            DocumentSourceSkip::create(pExpCtx));

        pSkip->_skip = pBsonElement->numberLong();
        uassert(15956, str::stream() << DocumentSourceSkip::skipName <<
                ":  the number to skip cannot be negative",
                pSkip->_skip >= 0);

        return pSkip;
    }
}
