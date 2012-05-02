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
#include "db/pipeline/expression.h"
#include "db/pipeline/value.h"

namespace mongo {

    const char DocumentSourceFilter::filterName[] = "$filter";

    DocumentSourceFilter::~DocumentSourceFilter() {
    }

    const char *DocumentSourceFilter::getSourceName() const {
        return filterName;
    }

    bool DocumentSourceFilter::coalesce(
        const intrusive_ptr<DocumentSource> &pNextSource) {

        /* we only know how to coalesce other filters */
        DocumentSourceFilter *pDocFilter =
            dynamic_cast<DocumentSourceFilter *>(pNextSource.get());
        if (!pDocFilter)
            return false;

        /*
          Two adjacent filters can be combined by creating a conjunction of
          their predicates.
         */
        intrusive_ptr<ExpressionNary> pAnd(ExpressionAnd::create());
        pAnd->addOperand(pFilter);
        pAnd->addOperand(pDocFilter->pFilter);
        pFilter = pAnd;

        return true;
    }

    void DocumentSourceFilter::optimize() {
        pFilter = pFilter->optimize();
    }

    void DocumentSourceFilter::sourceToBson(BSONObjBuilder *pBuilder) const {
        pFilter->addToBsonObj(pBuilder, filterName, false);
    }

    bool DocumentSourceFilter::accept(
        const intrusive_ptr<Document> &pDocument) const {
        intrusive_ptr<const Value> pValue(pFilter->evaluate(pDocument));
        return pValue->coerceToBool();
    }

    intrusive_ptr<DocumentSource> DocumentSourceFilter::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pCtx) {
        uassert(15946, "a document filter expression must be an object",
                pBsonElement->type() == Object);

        Expression::ObjectCtx oCtx(0);
        intrusive_ptr<Expression> pExpression(
            Expression::parseObject(pBsonElement, &oCtx));
        intrusive_ptr<DocumentSourceFilter> pFilter(
            DocumentSourceFilter::create(pExpression, pCtx));

        return pFilter;
    }

    intrusive_ptr<DocumentSourceFilter> DocumentSourceFilter::create(
        const intrusive_ptr<Expression> &pFilter,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceFilter> pSource(
            new DocumentSourceFilter(pFilter, pExpCtx));
        return pSource;
    }

    DocumentSourceFilter::DocumentSourceFilter(
        const intrusive_ptr<Expression> &pTheFilter,
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSourceFilterBase(pExpCtx),
        pFilter(pTheFilter) {
    }

    void DocumentSourceFilter::toMatcherBson(BSONObjBuilder *pBuilder) const {
        pFilter->toMatcherBson(pBuilder);
    }
}
