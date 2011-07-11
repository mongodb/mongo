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
    const char DocumentSourceSort::sortName[] = "$sort";

    DocumentSourceSort::~DocumentSourceSort() {
    }

    bool DocumentSourceSort::eof() {
        if (!populated)
            populate();

        return (listIterator == documents.end());
    }

    bool DocumentSourceSort::advance() {
        if (!populated)
            populate();

        assert(listIterator != documents.end()); // CW TODO error

        ++listIterator;
        if (listIterator == documents.end()) {
            pCurrent.reset();
            return false;
        }
	pCurrent = listIterator->pDocument;

        return true;
    }

    shared_ptr<Document> DocumentSourceSort::getCurrent() {
        if (!populated)
            populate();

        return pCurrent;
    }

    void DocumentSourceSort::sourceToBson(BSONObjBuilder *pBuilder) const {
	BSONObjBuilder insides;

	/* add the key fields */
	const size_t n = vSortKey.size();
	for(size_t i = 0; i < n; ++i) {
	    /* create the "field name" */
	    stringstream ss;
	    vSortKey[i]->writeFieldPath(ss, false);

	    /* append a named integer based on the sort order */
	    insides.append(ss.str(), (vSortKey[i] ? 1 : -1));
	}

	pBuilder->append(sortName, insides.done());
    }

    shared_ptr<DocumentSourceSort> DocumentSourceSort::create(
	const intrusive_ptr<ExpressionContext> &pCtx) {
        shared_ptr<DocumentSourceSort> pSource(
            new DocumentSourceSort(pCtx));
        return pSource;
    }

    DocumentSourceSort::DocumentSourceSort(
	const intrusive_ptr<ExpressionContext> &pTheCtx):
        populated(false),
        pCtx(pTheCtx) {
    }

    void DocumentSourceSort::addKey(const string &fieldPath, bool ascending) {
	shared_ptr<ExpressionFieldPath> pE(
	    ExpressionFieldPath::create(fieldPath));
	vSortKey.push_back(pE);
	vAscending.push_back(ascending);
    }

    shared_ptr<DocumentSource> DocumentSourceSort::createFromBson(
	BSONElement *pBsonElement,
	const intrusive_ptr<ExpressionContext> &pCtx) {
        assert(pBsonElement->type() == Object); // CW TODO must be an object

        shared_ptr<DocumentSourceSort> pSort(DocumentSourceSort::create(pCtx));

        BSONObj sortObj(pBsonElement->Obj());
        BSONObjIterator sortIterator(sortObj.getObjectField("key"));
        while(sortIterator.more()) {
            BSONElement sortField(sortIterator.next());
            const char *pFieldName = sortField.fieldName();
	    int sortOrder = 0;

	    switch(sortField.type()) {
	    case NumberInt:
		sortOrder = sortField.Int();
		break;

	    case NumberLong:
		sortOrder = (int)sortField.Long();
		break;

	    case NumberDouble:
		sortOrder = (int)sortField.Double();
		break;

	    default:
		assert(false); // CW TODO illegal sort order specification
		break;
	    }

	    assert(sortOrder != 0); // CW TODO illegal sort order value
	    pSort->addKey(pFieldName, (sortOrder > 0));
        }

        return pSort;
    }

    void DocumentSourceSort::populate() {
	/* make sure we've got a sort key */
	assert(vSortKey.size()); // CW TODO error

	/* pull everything from the underlying source */
        for(bool hasNext = !pSource->eof(); hasNext;
                hasNext = pSource->advance())
	    documents.push_back(Carrier(this, pSource->getCurrent()));

	/* sort the list */
	documents.sort(Carrier::lessThan);

        /* start the sort iterator */
        listIterator = documents.begin();
        if (listIterator != documents.end())
            pCurrent = listIterator->pDocument;
        populated = true;
    }

    int DocumentSourceSort::compare(
	const shared_ptr<Document> &pL, const shared_ptr<Document> &pR) {

	/*
	  populate() already checked that there is a non-empty sort key,
	  so we shouldn't have to worry about that here.

	  However, the tricky part is what to do is none of the sort keys are
	  present.  In this case, consider the document less.
	*/
	const size_t n = vSortKey.size();
	for(size_t i = 0; i < n; ++i) {
	    /* evaluate the sort keys */
	    ExpressionFieldPath *pE = vSortKey[i].get();
	    shared_ptr<const Value> pLeft(pE->evaluate(pL));
	    shared_ptr<const Value> pRight(pE->evaluate(pR));

	    /*
	      Compare the two values; if they differ, return.  If they are
	      the same, move on to the next key.
	    */
	    int cmp = Value::compare(pLeft, pRight);
	    if (cmp) {
		/* if necessary, adjust the return value by the key ordering */
		if (!vAscending[i])
		    cmp = -cmp;

		return cmp;
	    }
	}

	/*
	  If we got here, everything matched (or didn't exist), so we'll
	  consider the documents equal for purposes of this sort.
	*/
	return 0;
    }

    bool DocumentSourceSort::Carrier::lessThan(
	const Carrier &rL, const Carrier &rR) {
	/* make sure these aren't from different lists */
	assert(rL.pSort == rR.pSort);

	/* compare the documents according to the sort key */
	return (rL.pSort->compare(rL.pDocument, rR.pDocument) < 0);
    }
}
