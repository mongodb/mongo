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

#include "mongo/db/pipeline/document_source_sort.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_merge_cursors.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;
using std::unique_ptr;
using std::make_pair;
using std::string;
using std::vector;

DocumentSourceSort::DocumentSourceSort(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx), _mergingPresorted(false) {}

REGISTER_DOCUMENT_SOURCE(sort,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSort::createFromBson);

const char* DocumentSourceSort::getSourceName() const {
    return "$sort";
}

DocumentSource::GetNextResult DocumentSourceSort::getNext() {
    pExpCtx->checkForInterrupt();

    if (!_populated) {
        const auto populationResult = populate();
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());
    }

    if (!_output || !_output->more()) {
        // Need to be sure connections are marked as done so they can be returned to the connection
        // pool. This only needs to happen in the _mergingPresorted case, but it doesn't hurt to
        // always do it.
        dispose();
        return GetNextResult::makeEOF();
    }

    return _output->next().second;
}

void DocumentSourceSort::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {  // always one Value for combined $sort + $limit
        array.push_back(Value(
            DOC(getSourceName() << DOC(
                    "sortKey" << serializeSortKey(static_cast<bool>(explain)) << "mergePresorted"
                              << (_mergingPresorted ? Value(true) : Value())
                              << "limit"
                              << (limitSrc ? Value(limitSrc->getLimit()) : Value())))));
    } else {  // one Value for $sort and maybe a Value for $limit
        MutableDocument inner(serializeSortKey(static_cast<bool>(explain)));
        if (_mergingPresorted)
            inner["$mergePresorted"] = Value(true);
        array.push_back(Value(DOC(getSourceName() << inner.freeze())));

        if (limitSrc) {
            limitSrc->serializeToArray(array);
        }
    }
}

void DocumentSourceSort::doDispose() {
    _output.reset();
}

long long DocumentSourceSort::getLimit() const {
    return limitSrc ? limitSrc->getLimit() : -1;
}

Document DocumentSourceSort::serializeSortKey(bool explain) const {
    MutableDocument keyObj;
    const size_t n = _sortPattern.size();
    for (size_t i = 0; i < n; ++i) {
        if (_sortPattern[i].fieldPath) {
            // Append a named integer based on whether the sort is ascending/descending.
            keyObj.setField(_sortPattern[i].fieldPath->fullPath(),
                            Value(_sortPattern[i].isAscending ? 1 : -1));
        } else {
            // For sorts that are not simply on a field path, use a made-up field name.
            keyObj[string(str::stream() << "$computed" << i)] =
                _sortPattern[i].expression->serialize(explain);
        }
    }
    return keyObj.freeze();
}

Pipeline::SourceContainer::iterator DocumentSourceSort::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        // If the following stage is a $limit, we can combine it with ourselves.
        setLimitSrc(nextLimit);
        container->erase(std::next(itr));
        return itr;
    }
    return std::next(itr);
}

DocumentSource::GetDepsReturn DocumentSourceSort::getDependencies(DepsTracker* deps) const {
    for (auto&& keyPart : _sortPattern) {
        if (keyPart.expression) {
            keyPart.expression->addDependencies(deps);
        } else {
            deps->fields.insert(keyPart.fieldPath->fullPath());
        }
    }

    return SEE_NEXT;
}


intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15973, "the $sort key specification must be an object", elem.type() == Object);
    return create(pExpCtx, elem.embeddedObject());
}

intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    BSONObj sortOrder,
    long long limit,
    uint64_t maxMemoryUsageBytes) {
    intrusive_ptr<DocumentSourceSort> pSort(new DocumentSourceSort(pExpCtx));
    pSort->_maxMemoryUsageBytes = maxMemoryUsageBytes;
    pSort->_rawSort = sortOrder.getOwned();

    for (auto&& keyField : sortOrder) {
        auto fieldName = keyField.fieldNameStringData();

        if ("$mergePresorted" == fieldName) {
            verify(keyField.Bool());
            pSort->_mergingPresorted = true;
            continue;
        }

        SortPatternPart patternPart;

        if (keyField.type() == Object) {
            BSONObj metaDoc = keyField.Obj();
            // this restriction is due to needing to figure out sort direction
            uassert(17312,
                    "$meta is the only expression supported by $sort right now",
                    metaDoc.firstElement().fieldNameStringData() == "$meta");

            uassert(ErrorCodes::FailedToParse,
                    "Cannot have additional keys in a $meta sort specification",
                    metaDoc.nFields() == 1);

            VariablesParseState vps = pExpCtx->variablesParseState;
            patternPart.expression = ExpressionMeta::parse(pExpCtx, metaDoc.firstElement(), vps);

            // If sorting by textScore, sort highest scores first. If sorting by randVal, order
            // doesn't matter, so just always use descending.
            patternPart.isAscending = false;

            pSort->_sortPattern.push_back(std::move(patternPart));
            continue;
        }

        uassert(15974,
                "$sort key ordering must be specified using a number or {$meta: 'textScore'}",
                keyField.isNumber());

        int sortOrder = keyField.numberInt();

        uassert(15975,
                "$sort key ordering must be 1 (for ascending) or -1 (for descending)",
                ((sortOrder == 1) || (sortOrder == -1)));

        patternPart.fieldPath = FieldPath{fieldName};
        patternPart.isAscending = (sortOrder > 0);
        pSort->_paths.insert(patternPart.fieldPath->fullPath());
        pSort->_sortPattern.push_back(std::move(patternPart));
    }

    uassert(15976, "$sort stage must have at least one sort key", !pSort->_sortPattern.empty());

    const bool isExplain = false;
    pSort->_sortKeyGen =
        SortKeyGenerator{pSort->serializeSortKey(isExplain).toBson(), pExpCtx->getCollator()};

    if (limit > 0) {
        pSort->setLimitSrc(DocumentSourceLimit::create(pExpCtx, limit));
    }

    return pSort;
}

SortOptions DocumentSourceSort::makeSortOptions() const {
    /* make sure we've got a sort key */
    verify(_sortPattern.size());

    SortOptions opts;
    if (limitSrc)
        opts.limit = limitSrc->getLimit();

    opts.maxMemoryUsageBytes = _maxMemoryUsageBytes;
    if (pExpCtx->extSortAllowed && !pExpCtx->inRouter) {
        opts.extSortAllowed = true;
        opts.tempDir = pExpCtx->tempDir;
    }

    return opts;
}

DocumentSource::GetNextResult DocumentSourceSort::populate() {
    if (_mergingPresorted) {
        typedef DocumentSourceMergeCursors DSCursors;
        if (DSCursors* castedSource = dynamic_cast<DSCursors*>(pSource)) {
            populateFromCursors(castedSource->getCursors());
        } else {
            msgasserted(17196, "can only mergePresorted from MergeCursors");
        }
        return DocumentSource::GetNextResult::makeEOF();
    } else {
        auto nextInput = pSource->getNext();
        for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
            loadDocument(nextInput.releaseDocument());
        }
        if (nextInput.isEOF()) {
            loadingDone();
        }
        return nextInput;
    }
}

void DocumentSourceSort::loadDocument(const Document& doc) {
    invariant(!_populated);
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
    _populated = true;
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
    _populated = true;
}

StatusWith<Value> DocumentSourceSort::extractKeyPart(const Document& doc,
                                                     const SortPatternPart& patternPart) const {
    if (patternPart.fieldPath) {
        invariant(!patternPart.expression);
        return document_path_support::extractElementAlongNonArrayPath(doc, *patternPart.fieldPath);
    } else {
        invariant(patternPart.expression);
        return patternPart.expression->evaluate(doc);
    }
}

StatusWith<Value> DocumentSourceSort::extractKeyFast(const Document& doc) const {
    if (_sortPattern.size() == 1u) {
        return extractKeyPart(doc, _sortPattern[0]);
    }

    vector<Value> keys;
    keys.reserve(_sortPattern.size());
    for (auto&& keyPart : _sortPattern) {
        auto extractedKey = extractKeyPart(doc, keyPart);
        if (!extractedKey.isOK()) {
            // We can't use the fast path, so bail out.
            return extractedKey;
        }

        keys.push_back(std::move(extractedKey.getValue()));
    }
    return Value{std::move(keys)};
}

Value DocumentSourceSort::extractKeyWithArray(const Document& doc) const {
    SortKeyGenerator::Metadata metadata;
    if (doc.hasTextScore()) {
        metadata.textScore = doc.getTextScore();
    }
    if (doc.hasRandMetaField()) {
        metadata.randVal = doc.getRandMetaField();
    }

    // Convert the Document to a BSONObj, but only do the conversion for the paths we actually need.
    // Then run the result through the SortKeyGenerator to obtain the final sort key.
    auto bsonDoc = document_path_support::documentToBsonWithPaths(doc, _paths);
    auto bsonKey = uassertStatusOK(_sortKeyGen->getSortKey(std::move(bsonDoc), &metadata));

    // Convert BSON sort key, which is an object with empty field names, into the corresponding
    // array of keys representation as a Value. BSONObj {'': 1, '': [2, 3]} becomes Value [1, [2,
    // 3]].
    vector<Value> keys;
    keys.reserve(_sortPattern.size());
    for (auto&& elt : bsonKey) {
        keys.push_back(Value{elt});
    }
    return Value{std::move(keys)};
}

Value DocumentSourceSort::extractKey(const Document& doc) const {
    auto key = extractKeyFast(doc);
    if (key.isOK()) {
        return key.getValue();
    }

    return extractKeyWithArray(doc);
}

int DocumentSourceSort::compare(const Value& lhs, const Value& rhs) const {
    /*
      populate() already checked that there is a non-empty sort key,
      so we shouldn't have to worry about that here.

      However, the tricky part is what to do is none of the sort keys are
      present.  In this case, consider the document less.
    */
    const size_t n = _sortPattern.size();
    if (n == 1) {  // simple fast case
        if (_sortPattern[0].isAscending)
            return pExpCtx->getValueComparator().compare(lhs, rhs);
        else
            return -pExpCtx->getValueComparator().compare(lhs, rhs);
    }

    // compound sort
    for (size_t i = 0; i < n; i++) {
        int cmp = pExpCtx->getValueComparator().compare(lhs[i], rhs[i]);
        if (cmp) {
            /* if necessary, adjust the return value by the key ordering */
            if (!_sortPattern[i].isAscending)
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
    other->_sortPattern = _sortPattern;
    other->limitSrc = limitSrc;
    other->_mergingPresorted = true;
    other->_rawSort = _rawSort;
    return other;
}
}

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
