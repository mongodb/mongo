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

#include "pch.h"

#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/expression_context.h"
#include "db/pipeline/value.h"

namespace mongo {
    const char DocumentSourceSort::sortName[] = "$sort";

    const char *DocumentSourceSort::getSourceName() const {
        return sortName;
    }

    boost::optional<Document> DocumentSourceSort::getNext() {
        pExpCtx->checkForInterrupt();

        if (!populated)
            populate();

        if (!_output || !_output->more())
            return boost::none;

        return _output->next().second;
    }

    void DocumentSourceSort::serializeToArray(vector<Value>& array, bool explain) const {
        if (explain) { // always one Value for combined $sort + $limit
            array.push_back(Value(DOC(getSourceName() <<
                DOC("sortKey" << serializeSortKey()
                 << "mergePresorted" << (_mergingPresorted ? Value(true) : Value())
                 << "limit" << (limitSrc ? Value(limitSrc->getLimit()) : Value())))));
        }
        else { // one Value for $sort and maybe a Value for $limit
            MutableDocument inner (serializeSortKey());
            if (_mergingPresorted)
                inner["$mergePresorted"] = Value(true);
            array.push_back(Value(DOC(getSourceName() << inner.freeze())));

            if (limitSrc) {
                limitSrc->serializeToArray(array);
            }
        }
    }

    void DocumentSourceSort::dispose() {
        _output.reset();
        pSource->dispose();
    }

    DocumentSourceSort::DocumentSourceSort(const intrusive_ptr<ExpressionContext> &pExpCtx)
        : DocumentSource(pExpCtx)
        , populated(false)
        , _mergingPresorted(false)
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

    Document DocumentSourceSort::serializeSortKey() const {
        MutableDocument keyObj;
        // add the key fields
        const size_t n = vSortKey.size();
        for(size_t i = 0; i < n; ++i) {
            // get the field name out of each ExpressionFieldPath
            const FieldPath& withVariable = vSortKey[i]->getFieldPath();
            verify(withVariable.getPathLength() > 1);
            verify(withVariable.getFieldName(0) == "ROOT");
            const string fieldPath = withVariable.tail().getPath(false);

            // append a named integer based on the sort order
            keyObj.setField(fieldPath, Value(vAscending[i] ? 1 : -1));
        }
        return keyObj.freeze();
    }

    DocumentSource::GetDepsReturn DocumentSourceSort::getDependencies(set<string>& deps) const {
        for(size_t i = 0; i < vSortKey.size(); ++i) {
            vSortKey[i]->addDependencies(deps);
        }

        return SEE_NEXT;
    }


    intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        uassert(15973, str::stream() << " the " <<
                sortName << " key specification must be an object",
                elem.type() == Object);

        return create(pExpCtx, elem.embeddedObject());
    }

    intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
            const intrusive_ptr<ExpressionContext> &pExpCtx,
            BSONObj sortOrder,
            long long limit) {

        intrusive_ptr<DocumentSourceSort> pSort = new DocumentSourceSort(pExpCtx);

        /* check for then iterate over the sort object */
        size_t sortKeys = 0;
        BSONForEach(keyField, sortOrder) {
            const char* fieldName = keyField.fieldName();

            if (str::equals(fieldName, "$mergePresorted")) {
                verify(keyField.Bool());
                pSort->_mergingPresorted = true;
                continue;
            }
                
            uassert(15974, "$sort key ordering must be specified using a number",
                    keyField.isNumber());

            int sortOrder = keyField.numberInt();

            uassert(15975, "$sort key ordering must be 1 (for ascending) or -1 (for descending)",
                    ((sortOrder == 1) || (sortOrder == -1)));

            pSort->addKey(fieldName, (sortOrder > 0));
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

    SortOptions DocumentSourceSort::makeSortOptions() const {
        /* make sure we've got a sort key */
        verify(vSortKey.size());

        SortOptions opts;
        if (limitSrc)
            opts.limit = limitSrc->getLimit();

        opts.maxMemoryUsageBytes = 100*1024*1024;
        if (pExpCtx->extSortAllowed && !pExpCtx->inRouter) {
            opts.extSortAllowed = true;
            opts.tempDir = pExpCtx->tempDir;
        }

        return opts;
    }

    void DocumentSourceSort::populate() {
        if (_mergingPresorted) {
            typedef DocumentSourceMergeCursors DSCursors;
            typedef DocumentSourceCommandShards DSCommands;
            if (DSCursors* castedSource = dynamic_cast<DSCursors*>(pSource)) {
                populateFromCursors(castedSource->getCursors());
            } else if (DSCommands* castedSource = dynamic_cast<DSCommands*>(pSource)) {
                populateFromBsonArrays(castedSource->getArrays());
            } else {
                msgasserted(17196, "can only mergePresorted from MergeCursors and CommandShards");
            }
        } else {
            scoped_ptr<MySorter> sorter (MySorter::make(makeSortOptions(), Comparator(*this)));
            while (boost::optional<Document> next = pSource->getNext()) {
                sorter->add(extractKey(*next), *next);
            }
            _output.reset(sorter->done());
        }
        populated = true;
    }

    class DocumentSourceSort::IteratorFromCursor : public MySorter::Iterator {
    public:
        IteratorFromCursor(DocumentSourceSort* sorter, DBClientCursor* cursor)
            : _sorter(sorter)
            , _cursor(cursor)
        {}

        bool more() { return _cursor->more(); }
        Data next() {
            Document doc(_cursor->next());
            return make_pair(_sorter->extractKey(doc), doc);
        }
    private:
        DocumentSourceSort* _sorter;
        DBClientCursor* _cursor;
    };

    void DocumentSourceSort::populateFromCursors(const vector<DBClientCursor*>& cursors) {
        vector<boost::shared_ptr<MySorter::Iterator> > iterators;
        for (size_t i = 0; i < cursors.size(); i++) {
            iterators.push_back(boost::make_shared<IteratorFromCursor>(this, cursors[i]));
        }

        _output.reset(MySorter::Iterator::merge(iterators, makeSortOptions(), Comparator(*this)));
    }

    class DocumentSourceSort::IteratorFromBsonArray : public MySorter::Iterator {
    public:
        IteratorFromBsonArray(DocumentSourceSort* sorter, const BSONArray& array)
            : _sorter(sorter)
            , _iterator(array)
        {}

        bool more() { return _iterator.more(); }
        Data next() {
            Document doc(_iterator.next().Obj());
            return make_pair(_sorter->extractKey(doc), doc);
        }
    private:
        DocumentSourceSort* _sorter;
        BSONObjIterator _iterator;
    };

    void DocumentSourceSort::populateFromBsonArrays(const vector<BSONArray>& arrays) {
        vector<boost::shared_ptr<MySorter::Iterator> > iterators;
        for (size_t i = 0; i < arrays.size(); i++) {
            iterators.push_back(boost::make_shared<IteratorFromBsonArray>(this, arrays[i]));
        }

        _output.reset(MySorter::Iterator::merge(iterators, makeSortOptions(), Comparator(*this)));
    }

    Value DocumentSourceSort::extractKey(const Document& d) const {
        if (vSortKey.size() == 1) {
            return vSortKey[0]->evaluate(d);
        }

        const Variables vars(d);
        vector<Value> keys;
        keys.reserve(vSortKey.size());
        for (size_t i=0; i < vSortKey.size(); i++) {
            keys.push_back(vSortKey[i]->evaluate(vars));
        }
        return Value::consume(keys);
    }

    int DocumentSourceSort::compare(const Value& lhs, const Value& rhs) const {

        /*
          populate() already checked that there is a non-empty sort key,
          so we shouldn't have to worry about that here.

          However, the tricky part is what to do is none of the sort keys are
          present.  In this case, consider the document less.
        */
        const size_t n = vSortKey.size();
        if (n == 1) { // simple fast case
            if (vAscending[0])
                return  Value::compare(lhs, rhs);
            else
                return -Value::compare(lhs, rhs);
        }

        // compound sort
        for (size_t i = 0; i < n; i++) {
            int cmp = Value::compare(lhs[i], rhs[i]);
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

    intrusive_ptr<DocumentSource> DocumentSourceSort::getShardSource() {
        verify(!_mergingPresorted);
        return this;
    }

    intrusive_ptr<DocumentSource> DocumentSourceSort::getMergeSource() {
        verify(!_mergingPresorted);
        intrusive_ptr<DocumentSourceSort> other = new DocumentSourceSort(pExpCtx);
        other->vAscending = vAscending;
        other->vSortKey = vSortKey;
        other->limitSrc = limitSrc;
        other->_mergingPresorted = true;
        return other;
    }
}

#include "db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
