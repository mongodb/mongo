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

#include "db/pipeline/expression.h"
#include "db/pipeline/value.h"

namespace mongo {
    DocumentSourceFilter::~DocumentSourceFilter() {
    }

    void DocumentSourceFilter::findNext() {
        /* only do this the first time */
        if (unstarted) {
            hasNext = !pSource->eof();
            unstarted = false;
        }

        while(hasNext) {
            shared_ptr<Document> pDocument(pSource->getCurrent());
            hasNext = pSource->advance();

            shared_ptr<const Value> pValue(pFilter->evaluate(pDocument));
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

    shared_ptr<Document> DocumentSourceFilter::getCurrent() {
        if (unstarted)
            findNext();

        assert(pCurrent.get() != NULL); // CW TODO error
        return pCurrent;
    }

    shared_ptr<DocumentSourceFilter> DocumentSourceFilter::create(
        shared_ptr<Expression> pTheFilter,
        shared_ptr<DocumentSource> pTheSource) {
        shared_ptr<DocumentSourceFilter> pSource(
            new DocumentSourceFilter(pTheFilter, pTheSource));
        return pSource;
    }

    DocumentSourceFilter::DocumentSourceFilter(
        shared_ptr<Expression> pTheFilter,
        shared_ptr<DocumentSource> pTheSource):
        pSource(pTheSource),
        pFilter(pTheFilter),
        unstarted(true),
        hasNext(false),
        pCurrent() {
    }
}
