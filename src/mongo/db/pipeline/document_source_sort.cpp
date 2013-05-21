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

        return documents.empty();
    }

    bool DocumentSourceSort::advance() {
        DocumentSource::advance(); // check for interrupts

        if (!populated)
            populate();

        if (!documents.empty())
            documents.pop_front(); // this way we release memory as we go

        return !documents.empty();
    }

    Document DocumentSourceSort::getCurrent() {
        verify(!documents.empty());
        return documents.front().doc;
    }

    void DocumentSourceSort::addToBsonArray(BSONArrayBuilder *pBuilder, bool explain) const {
        if (explain) { // always one obj for combined $sort + $limit
            BSONObjBuilder sortObj (pBuilder->subobjStart());
            BSONObjBuilder insides (sortObj.subobjStart(sortName));
            BSONObjBuilder sortKey (insides.subobjStart("sortKey"));
            sortKeyToBson(&sortKey, false);
            sortKey.doneFast();

            if (explain && limitSrc) {
                insides.appendNumber("limit", limitSrc->getLimit());
            }
            insides.doneFast();
            sortObj.doneFast();
        }
        else { // one obj for $sort + maybe one obj for $limit
            {
                BSONObjBuilder sortObj (pBuilder->subobjStart());
                BSONObjBuilder insides (sortObj.subobjStart(sortName));
                sortKeyToBson(&insides, false);
                insides.doneFast();
                sortObj.doneFast();
            }

            if (limitSrc) {
                limitSrc->addToBsonArray(pBuilder, explain);
            }
        }
    }

    void DocumentSourceSort::dispose() {
        documents.clear();
        pSource->dispose();
    }

    DocumentSourceSort::DocumentSourceSort(const intrusive_ptr<ExpressionContext> &pExpCtx)
        : SplittableDocumentSource(pExpCtx)
        , populated(false)
    {}

    long long DocumentSourceSort::getLimit() const {
        return limitSrc ? limitSrc->getLimit() : -1;
    }

    bool DocumentSourceSort::coalesce(const intrusive_ptr<DocumentSource> &pNextSource) {
        if (!limitSrc) {
            limitSrc = dynamic_cast<DocumentSourceLimit*>(pNextSource.get());
            return limitSrc; // false if next is not a $limit
        }
        else {
            return limitSrc->coalesce(pNextSource);
        }
    }

    void DocumentSourceSort::addKey(const string& fieldPath, bool ascending) {
        vSortKey.push_back(ExpressionFieldPath::parse("$$ROOT." + fieldPath));
        vAscending.push_back(ascending);
    }

    void DocumentSourceSort::sortKeyToBson(BSONObjBuilder* pBuilder, bool usePrefix) const {
        /* add the key fields */
        const size_t n = vSortKey.size();
        for(size_t i = 0; i < n; ++i) {
            // get the field name out of each ExpressionFieldPath
            const FieldPath& withVariable = vSortKey[i]->getFieldPath();
            verify(withVariable.getPathLength() > 1);
            verify(withVariable.getFieldName(0) == "ROOT");
            const string fieldPath = withVariable.tail().getPath(false);

            /* append a named integer based on the sort order */
            pBuilder->append(fieldPath, (vAscending[i] ? 1 : -1));
        }
    }
    DocumentSource::GetDepsReturn DocumentSourceSort::getDependencies(set<string>& deps) const {
        for(size_t i = 0; i < vSortKey.size(); ++i) {
            vSortKey[i]->addDependencies(deps);
        }

        return SEE_NEXT;
    }


    intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15973, str::stream() << " the " <<
                sortName << " key specification must be an object",
                pBsonElement->type() == Object);

        return create(pExpCtx, pBsonElement->embeddedObject());

    }

    intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            BSONObj sortOrder,
            long long limit) {

        intrusive_ptr<DocumentSourceSort> pSort = new DocumentSourceSort(pExpCtx);

        /* check for then iterate over the sort object */
        size_t sortKeys = 0;
        for(BSONObjIterator keyIterator(sortOrder);
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

        if (limit > 0) {
            bool coalesced = pSort->coalesce(DocumentSourceLimit::create(pExpCtx, limit));
            verify(coalesced); // should always coalesce
            verify(pSort->getLimit() == limit);
        }

        return pSort;
    }

    void DocumentSourceSort::populate() {
        /* make sure we've got a sort key */
        verify(vSortKey.size());

        if (!limitSrc)
            populateAll();
        else if (limitSrc->getLimit() == 1)
            populateOne();
        else
            populateTopK();

        populated = true;
    }

    void DocumentSourceSort::populateAll() {
        /* track and warn about how much physical memory has been used */
        DocMemMonitor dmm(this);

        /* pull everything from the underlying source */
        for (bool hasNext = !pSource->eof(); hasNext; hasNext = pSource->advance()) {
            documents.push_back(KeyAndDoc(pSource->getCurrent(), vSortKey));
            dmm.addToTotal(documents.back().doc.getApproximateSize());
        }

        /* sort the list */
        Comparator comparator(*this);
        sort(documents.begin(), documents.end(), comparator);
    }

    void DocumentSourceSort::populateOne() {
        if (pSource->eof())
            return;

        KeyAndDoc best (pSource->getCurrent(), vSortKey);
        while (pSource->advance()) {
            KeyAndDoc next (pSource->getCurrent(), vSortKey);
            if (compare(next, best) < 0) {
                // we have a new best
                swap(best, next);
            }
        }

        documents.push_back(best);
    }

    void DocumentSourceSort::populateTopK() {
        bool hasNext = !pSource->eof();

        size_t limit = limitSrc->getLimit();

        // Pull first K documents unconditionally
        vector<KeyAndDoc> heap;
        heap.reserve(limit);
        for (; hasNext && heap.size() < limit; hasNext = pSource->advance()) {
            heap.push_back(KeyAndDoc(pSource->getCurrent(), vSortKey));
        }

        // We now maintain a MaxHeap of K items. This means that the least-best
        // document is at the top of the heap (heap.front()). If a new
        // document is better than the top of the heap, we pop the top and add
        // the new document to the heap.

        Comparator comp (*this);

        // after this, heap.front() is least-best document
        std::make_heap(heap.begin(), heap.end(), comp);

        for (; hasNext; hasNext = pSource->advance()) {
            KeyAndDoc next (pSource->getCurrent(), vSortKey);
            if (compare(next, heap.front()) < 0) {
                // remove least-best from heap
                std::pop_heap(heap.begin(), heap.end(), comp);

                // add next to heap
                swap(heap.back(), next);
                std::push_heap(heap.begin(), heap.end(), comp);
            }
        }

        std::sort_heap(heap.begin(), heap.end(), comp);
        documents.insert(documents.begin(), heap.begin(), heap.end());
    }

    DocumentSourceSort::KeyAndDoc::KeyAndDoc(const Document& d, const SortPaths& sp) :doc(d) {
        if (sp.size() == 1) {
            key = sp[0]->evaluate(d);
            return;
        }

        const Variables vars(d);
        vector<Value> keys;
        keys.reserve(sp.size());
        for (size_t i=0; i < sp.size(); i++) {
            keys.push_back(sp[i]->evaluate(vars));
        }
        key = Value::consume(keys);
    }

    int DocumentSourceSort::compare(const KeyAndDoc & lhs, const KeyAndDoc & rhs) const {

        /*
          populate() already checked that there is a non-empty sort key,
          so we shouldn't have to worry about that here.

          However, the tricky part is what to do is none of the sort keys are
          present.  In this case, consider the document less.
        */
        const size_t n = vSortKey.size();
        if (n == 1) { // simple fast case
            if (vAscending[0])
                return  Value::compare(lhs.key, rhs.key);
            else
                return -Value::compare(lhs.key, rhs.key);
        }

        // compound sort
        for (size_t i = 0; i < n; i++) {
            int cmp = Value::compare(lhs.key[i], rhs.key[i]);
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
}
