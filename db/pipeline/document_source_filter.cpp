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

    void DocumentSourceFilter::findNext() {
        /* only do this the first time */
        if (unstarted) {
            hasNext = !pSource->eof();
            unstarted = false;
        }

        while(hasNext) {
            boost::shared_ptr<Document> pDocument(pSource->getCurrent());
            hasNext = pSource->advance();

            boost::shared_ptr<const Value> pValue(pFilter->evaluate(pDocument));
            bool pass = pValue->coerceToBool();
            if (pass) {
                pCurrent = pDocument;
                return;
            }
        }

        pCurrent.reset();
    }

    bool DocumentSourceFilter::eof() {
        if (unstarted)
            findNext();

        return (pCurrent.get() == NULL);
    }

    bool DocumentSourceFilter::advance() {
        if (unstarted)
            findNext();

        /*
          This looks weird after the above, but is correct.  Note that calling
          getCurrent() when first starting already yields the first document
          in the collection.  Calling advance() without using getCurrent()
          first will skip over the first item.
         */
        findNext();

        return (pCurrent.get() != NULL);
    }

    boost::shared_ptr<Document> DocumentSourceFilter::getCurrent() {
        if (unstarted)
            findNext();

        assert(pCurrent.get() != NULL); // CW TODO error
        return pCurrent;
    }

    bool DocumentSourceFilter::coalesce(
	boost::shared_ptr<DocumentSource> pNextSource) {

	/* we only know how to coalesce other filters */
	DocumentSourceFilter *pDocFilter =
	    dynamic_cast<DocumentSourceFilter *>(pNextSource.get());
	if (!pDocFilter)
	    return false;

	/*
	  Two adjacent filters can be combined by creating a conjunction of
	  their predicates.
	 */
	boost::shared_ptr<ExpressionNary> pAnd(ExpressionAnd::create());
	pAnd->addOperand(pFilter);
	pAnd->addOperand(pDocFilter->pFilter);
	pFilter = pAnd;

	return true;
    }

    void DocumentSourceFilter::optimize() {
	pFilter = pFilter->optimize();
    }

    void DocumentSourceFilter::sourceToBson(BSONObjBuilder *pBuilder) const {
	pFilter->addToBsonObj(pBuilder, filterName, true);
    }

    boost::shared_ptr<DocumentSourceFilter> DocumentSourceFilter::createFromBson(
	BSONElement *pBsonElement) {
        assert(pBsonElement->type() == Object);
        // CW TODO error: expression object must be an object

        boost::shared_ptr<Expression> pExpression(
	    Expression::parseObject(pBsonElement,
				    &Expression::ObjectCtx(0)));
        boost::shared_ptr<DocumentSourceFilter> pFilter(
            DocumentSourceFilter::create(pExpression));

        return pFilter;
    }

    boost::shared_ptr<DocumentSourceFilter> DocumentSourceFilter::create(
        boost::shared_ptr<Expression> pFilter) {
        boost::shared_ptr<DocumentSourceFilter> pSource(
            new DocumentSourceFilter(pFilter));
        return pSource;
    }

    DocumentSourceFilter::DocumentSourceFilter(
        boost::shared_ptr<Expression> pTheFilter):
        pFilter(pTheFilter),
        unstarted(true),
        hasNext(false),
        pCurrent() {
    }

    void DocumentSourceFilter::toMatcherBson(BSONObjBuilder *pBuilder) const {
	pFilter->toMatcherBson(pBuilder);
    }
}
