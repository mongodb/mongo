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
#include "db/pipeline/dependency_tracker.h"
#include "db/pipeline/doc_mem_monitor.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"


namespace mongo {
    const char DocumentSourceSort::sortName[] = "$sort";

    DocumentSourceSort::~DocumentSourceSort() {
    }

    const char *DocumentSourceSort::getSourceName() const {
        return sortName;
    }

    bool DocumentSourceSort::eof() {
        if (!populated)
            populate();

        return (listIterator == documents.end());
    }

    bool DocumentSourceSort::advance() {
        DocumentSource::advance(); // check for interrupts

        if (!populated)
            populate();

        verify(listIterator != documents.end());

        ++listIterator;
        if (listIterator == documents.end()) {
            pCurrent.reset();
            count = 0;
            return false;
        }
        pCurrent = listIterator->pDocument;

        return true;
    }

    intrusive_ptr<Document> DocumentSourceSort::getCurrent() {
        if (!populated)
            populate();

        return pCurrent;
    }

    void DocumentSourceSort::sourceToBson(BSONObjBuilder *pBuilder) const {
        BSONObjBuilder insides;
        sortKeyToBson(&insides, false);
        pBuilder->append(sortName, insides.done());
    }

    intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceSort> pSource(
            new DocumentSourceSort(pExpCtx));
        return pSource;
    }

    DocumentSourceSort::DocumentSourceSort(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        populated(false) {
    }

    void DocumentSourceSort::addKey(const string &fieldPath, bool ascending) {
        intrusive_ptr<ExpressionFieldPath> pE(
            ExpressionFieldPath::create(fieldPath));
        vSortKey.push_back(pE);
        vAscending.push_back(ascending);
    }

    void DocumentSourceSort::sortKeyToBson(
        BSONObjBuilder *pBuilder, bool usePrefix) const {
        /* add the key fields */
        const size_t n = vSortKey.size();
        for(size_t i = 0; i < n; ++i) {
            /* create the "field name" */
            stringstream ss;
            vSortKey[i]->writeFieldPath(ss, usePrefix);

            /* append a named integer based on the sort order */
            pBuilder->append(ss.str(), (vAscending[i] ? 1 : -1));
        }
    }

    intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15973, str::stream() << " the " <<
                sortName << " key specification must be an object",
                pBsonElement->type() == Object);

        intrusive_ptr<DocumentSourceSort> pSort(
            DocumentSourceSort::create(pExpCtx));

        /* check for then iterate over the sort object */
        size_t sortKeys = 0;
        for(BSONObjIterator keyIterator(pBsonElement->Obj().begin());
            keyIterator.more();) {
            BSONElement keyField(keyIterator.next());
            const char *pKeyFieldName = keyField.fieldName();
            int sortOrder = 0;
                
            uassert(15974, str::stream() << sortName <<
                    " key ordering must be specified using a number",
                    keyField.isNumber());
            sortOrder = (int)keyField.numberInt();

            uassert(15975,  str::stream() << sortName <<
                    " key ordering must be 1 (for ascending) or -1 (for descending",
                    ((sortOrder == 1) || (sortOrder == -1)));

            pSort->addKey(pKeyFieldName, (sortOrder > 0));
            ++sortKeys;
        }

        uassert(15976, str::stream() << sortName <<
                " must have at least one sort key", (sortKeys > 0));

        return pSort;
    }

    void DocumentSourceSort::populate() {
        /* make sure we've got a sort key */
        verify(vSortKey.size());

        /* track and warn about how much physical memory has been used */
        DocMemMonitor dmm(this);

        /* pull everything from the underlying source */
        for(bool hasNext = !pSource->eof(); hasNext;
            hasNext = pSource->advance()) {
            intrusive_ptr<Document> pDocument(pSource->getCurrent());
            documents.push_back(Carrier(this, pDocument));

            dmm.addToTotal(pDocument->getApproximateSize());
        }

        /* sort the list */
        documents.sort(Carrier::lessThan);

        /* start the sort iterator */
        listIterator = documents.begin();

        if (listIterator != documents.end())
            pCurrent = listIterator->pDocument;
        populated = true;
    }

    int DocumentSourceSort::compare(
        const intrusive_ptr<Document> &pL, const intrusive_ptr<Document> &pR) {

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
            intrusive_ptr<const Value> pLeft(pE->evaluate(pL));
            intrusive_ptr<const Value> pRight(pE->evaluate(pR));

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
        verify(rL.pSort == rR.pSort);

        /* compare the documents according to the sort key */
        return (rL.pSort->compare(rL.pDocument, rR.pDocument) < 0);
    }

    void DocumentSourceSort::manageDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker) {
        /* get the dependencies out of the matcher */
        for(SortPaths::iterator i(vSortKey.begin()); i != vSortKey.end(); ++i) {
            string fieldPath((*i)->getFieldPath(false));
            pTracker->addDependency(fieldPath, this);
        }
    }

}
