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

#include "mongo/db/pipeline/document_source.h"


#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;
using std::unique_ptr;
using std::make_pair;
using std::string;
using std::vector;

DocumentSourceSort::DocumentSourceSort(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), populated(false), _mergingPresorted(false) {}

REGISTER_DOCUMENT_SOURCE(sort, DocumentSourceSort::createFromBson);

const char* DocumentSourceSort::getSourceName() const {
    return "$sort";
}

boost::optional<Document> DocumentSourceSort::getNext() {
    pExpCtx->checkForInterrupt();

    if (!populated)
        populate();

    if (!_output || !_output->more()) {
        // Need to be sure connections are marked as done so they can be returned to the connection
        // pool. This only needs to happen in the _mergingPresorted case, but it doesn't hurt to
        // always do it.
        dispose();
        return boost::none;
    }

    return _output->next().second;
}

void DocumentSourceSort::serializeToArray(vector<Value>& array, bool explain) const {
    if (explain) {  // always one Value for combined $sort + $limit
        array.push_back(
            Value(DOC(getSourceName()
                      << DOC("sortKey" << serializeSortKey(explain) << "mergePresorted"
                                       << (_mergingPresorted ? Value(true) : Value())
                                       << "limit"
                                       << (limitSrc ? Value(limitSrc->getLimit()) : Value())))));
    } else {  // one Value for $sort and maybe a Value for $limit
        MutableDocument inner(serializeSortKey(explain));
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
    if (pSource) {
        pSource->dispose();
    }
}

long long DocumentSourceSort::getLimit() const {
    return limitSrc ? limitSrc->getLimit() : -1;
}

void DocumentSourceSort::addKey(const string& fieldPath, bool ascending) {
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);
    vSortKey.push_back(ExpressionFieldPath::parse("$$ROOT." + fieldPath, vps));
    vAscending.push_back(ascending);
}

Document DocumentSourceSort::serializeSortKey(bool explain) const {
    MutableDocument keyObj;
    // add the key fields
    const size_t n = vSortKey.size();
    for (size_t i = 0; i < n; ++i) {
        if (ExpressionFieldPath* efp = dynamic_cast<ExpressionFieldPath*>(vSortKey[i].get())) {
            // ExpressionFieldPath gets special syntax that includes direction
            const FieldPath& withVariable = efp->getFieldPath();
            verify(withVariable.getPathLength() > 1);
            verify(withVariable.getFieldName(0) == "ROOT");
            const string fieldPath = withVariable.tail().fullPath();

            // append a named integer based on the sort order
            keyObj.setField(fieldPath, Value(vAscending[i] ? 1 : -1));
        } else {
            // other expressions use a made-up field name
            keyObj[string(str::stream() << "$computed" << i)] = vSortKey[i]->serialize(explain);
        }
    }
    return keyObj.freeze();
}

Pipeline::SourceContainer::iterator DocumentSourceSort::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());
    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        // If the following stage is a $limit, we can combine it with ourselves.
        setLimitSrc(nextLimit);
        container->erase(std::next(itr));
        return itr;
    } else if (nextMatch && !nextMatch->isTextQuery()) {
        // Swap the $match before the $sort, thus reducing the number of documents that pass into
        // this stage.
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

DocumentSource::GetDepsReturn DocumentSourceSort::getDependencies(DepsTracker* deps) const {
    for (size_t i = 0; i < vSortKey.size(); ++i) {
        vSortKey[i]->addDependencies(deps);
    }

    return SEE_NEXT;
}


intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15973, "the $sort key specification must be an object", elem.type() == Object);
    return create(pExpCtx, elem.embeddedObject());
}

intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx, BSONObj sortOrder, long long limit) {
    intrusive_ptr<DocumentSourceSort> pSort = new DocumentSourceSort(pExpCtx);
    pSort->injectExpressionContext(pExpCtx);
    pSort->_sort = sortOrder.getOwned();

    /* check for then iterate over the sort object */
    BSONForEach(keyField, sortOrder) {
        const char* fieldName = keyField.fieldName();

        if (str::equals(fieldName, "$mergePresorted")) {
            verify(keyField.Bool());
            pSort->_mergingPresorted = true;
            continue;
        }

        if (keyField.type() == Object) {
            BSONObj metaDoc = keyField.Obj();
            // this restriction is due to needing to figure out sort direction
            uassert(17312,
                    "$meta is the only expression supported by $sort right now",
                    metaDoc.firstElement().fieldNameStringData() == "$meta");

            VariablesIdGenerator idGen;
            VariablesParseState vps(&idGen);
            pSort->vSortKey.push_back(ExpressionMeta::parse(metaDoc.firstElement(), vps));

            // If sorting by textScore, sort highest scores first. If sorting by randVal, order
            // doesn't matter, so just always use descending.
            pSort->vAscending.push_back(false);
            continue;
        }

        uassert(15974,
                "$sort key ordering must be specified using a number or {$meta: 'textScore'}",
                keyField.isNumber());

        int sortOrder = keyField.numberInt();

        uassert(15975,
                "$sort key ordering must be 1 (for ascending) or -1 (for descending)",
                ((sortOrder == 1) || (sortOrder == -1)));

        pSort->addKey(fieldName, (sortOrder > 0));
    }

    uassert(15976, "$sort stage must have at least one sort key", !pSort->vSortKey.empty());

    if (limit > 0) {
        pSort->setLimitSrc(DocumentSourceLimit::create(pExpCtx, limit));
    }

    return pSort;
}

SortOptions DocumentSourceSort::makeSortOptions() const {
    /* make sure we've got a sort key */
    verify(vSortKey.size());

    SortOptions opts;
    if (limitSrc)
        opts.limit = limitSrc->getLimit();

    opts.maxMemoryUsageBytes = 100 * 1024 * 1024;
    if (pExpCtx->extSortAllowed && !pExpCtx->inRouter) {
        opts.extSortAllowed = true;
        opts.tempDir = pExpCtx->tempDir;
    }

    return opts;
}

void DocumentSourceSort::populate() {
    if (_mergingPresorted) {
        typedef DocumentSourceMergeCursors DSCursors;
        if (DSCursors* castedSource = dynamic_cast<DSCursors*>(pSource)) {
            populateFromCursors(castedSource->getCursors());
        } else {
            msgasserted(17196, "can only mergePresorted from MergeCursors");
        }
    } else {
        while (boost::optional<Document> next = pSource->getNext()) {
            loadDocument(std::move(*next));
        }
        loadingDone();
    }
}

void DocumentSourceSort::loadDocument(const Document& doc) {
    invariant(!populated);
    if (!_sorter) {
        _sorter.reset(MySorter::make(makeSortOptions(), Comparator(*this)));
    }
    _sorter->add(extractKey(doc), doc);
}

void DocumentSourceSort::loadingDone() {
    if (!_sorter) {
        _sorter.reset(MySorter::make(makeSortOptions(), Comparator(*this)));
    }
    _output.reset(_sorter->done());
    _sorter.reset();
    populated = true;
}

class DocumentSourceSort::IteratorFromCursor : public MySorter::Iterator {
public:
    IteratorFromCursor(DocumentSourceSort* sorter, DBClientCursor* cursor)
        : _sorter(sorter), _cursor(cursor) {}

    bool more() {
        return _cursor->more();
    }
    Data next() {
        const Document doc = DocumentSourceMergeCursors::nextSafeFrom(_cursor);
        return make_pair(_sorter->extractKey(doc), doc);
    }

private:
    DocumentSourceSort* _sorter;
    DBClientCursor* _cursor;
};

void DocumentSourceSort::populateFromCursors(const vector<DBClientCursor*>& cursors) {
    vector<std::shared_ptr<MySorter::Iterator>> iterators;
    for (size_t i = 0; i < cursors.size(); i++) {
        iterators.push_back(std::make_shared<IteratorFromCursor>(this, cursors[i]));
    }

    _output.reset(MySorter::Iterator::merge(iterators, makeSortOptions(), Comparator(*this)));
    populated = true;
}

Value DocumentSourceSort::extractKey(const Document& d) const {
    Variables vars(0, d);
    if (vSortKey.size() == 1) {
        return vSortKey[0]->evaluate(&vars);
    }

    vector<Value> keys;
    keys.reserve(vSortKey.size());
    for (size_t i = 0; i < vSortKey.size(); i++) {
        keys.push_back(vSortKey[i]->evaluate(&vars));
    }
    return Value(std::move(keys));
}

int DocumentSourceSort::compare(const Value& lhs, const Value& rhs) const {
    /*
      populate() already checked that there is a non-empty sort key,
      so we shouldn't have to worry about that here.

      However, the tricky part is what to do is none of the sort keys are
      present.  In this case, consider the document less.
    */
    const size_t n = vSortKey.size();
    if (n == 1) {  // simple fast case
        if (vAscending[0])
            return Value::compare(lhs, rhs);
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
    other->_sort = _sort;
    return other;
}
}

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
