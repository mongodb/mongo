/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/query/document_source_merge_cursors.h"

namespace mongo {

using boost::intrusive_ptr;
using std::make_pair;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {

Value missingToNull(Value maybeMissing) {
    return maybeMissing.missing() ? Value(BSONNULL) : maybeMissing;
}

/**
 * Converts a Value representing an in-memory sort key to a BSONObj representing a serialized sort
 * key. If 'sortPatternSize' is 1, returns a BSON object with 'value' as it's only value - and an
 * empty field name. Otherwise asserts that 'value' is an array of length 'sortPatternSize', and
 * returns a BSONObj with one field for each value in the array, each field using the empty field
 * name.
 */
BSONObj serializeSortKey(size_t sortPatternSize, Value value) {
    // Missing values don't serialize correctly in this format, so use nulls instead, since they are
    // considered equivalent with woCompare().
    if (sortPatternSize == 1) {
        return BSON("" << missingToNull(value));
    }
    invariant(value.isArray());
    invariant(value.getArrayLength() == sortPatternSize);
    BSONObjBuilder bb;
    for (auto&& val : value.getArray()) {
        bb << "" << missingToNull(val);
    }
    return bb.obj();
}

/**
 * Converts a BSONObj representing a serialized sort key into a Value, which we use for in-memory
 * comparisons. BSONObj {'': 1, '': [2, 3]} becomes Value [1, [2, 3]].
 */
Value deserializeSortKey(size_t sortPatternSize, BSONObj bsonSortKey) {
    vector<Value> keys;
    keys.reserve(sortPatternSize);
    for (auto&& elt : bsonSortKey) {
        keys.push_back(Value{elt});
    }
    invariant(keys.size() == sortPatternSize);
    if (sortPatternSize == 1) {
        // As a special case for a sort on a single field, we do not put the keys into an array.
        return keys[0];
    }
    return Value{std::move(keys)};
}

/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> documentSourceSortFileCounter;
    return "extsort-doc-source-sort." +
        std::to_string(documentSourceSortFileCounter.fetchAndAdd(1));
}

}  // namespace

constexpr StringData DocumentSourceSort::kStageName;

DocumentSourceSort::DocumentSourceSort(const intrusive_ptr<ExpressionContext>& pExpCtx)
    : DocumentSource(pExpCtx) {}

REGISTER_DOCUMENT_SOURCE(sort,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSort::createFromBson);

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
        dispose();
        return GetNextResult::makeEOF();
    }

    return _output->next().second;
}

void DocumentSourceSort::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {  // always one Value for combined $sort + $limit
        array.push_back(
            Value(DOC(kStageName << DOC(
                          "sortKey" << sortKeyPattern(SortKeySerialization::kForExplain) << "limit"
                                    << (_limitSrc ? Value(_limitSrc->getLimit()) : Value())))));
    } else {  // one Value for $sort and maybe a Value for $limit
        MutableDocument inner(sortKeyPattern(SortKeySerialization::kForPipelineSerialization));
        array.push_back(Value(DOC(kStageName << inner.freeze())));

        if (_limitSrc) {
            _limitSrc->serializeToArray(array);
        }
    }
}

void DocumentSourceSort::doDispose() {
    _output.reset();
}

long long DocumentSourceSort::getLimit() const {
    return _limitSrc ? _limitSrc->getLimit() : -1;
}

Document DocumentSourceSort::sortKeyPattern(SortKeySerialization serializationMode) const {
    MutableDocument keyObj;
    const size_t n = _sortPattern.size();
    for (size_t i = 0; i < n; ++i) {
        if (_sortPattern[i].fieldPath) {
            // Append a named integer based on whether the sort is ascending/descending.
            keyObj.setField(_sortPattern[i].fieldPath->fullPath(),
                            Value(_sortPattern[i].isAscending ? 1 : -1));
        } else {
            // Sorting by an expression, use a made up field name.
            auto computedFieldName = string(str::stream() << "$computed" << i);
            switch (serializationMode) {
                case SortKeySerialization::kForExplain:
                case SortKeySerialization::kForPipelineSerialization: {
                    const bool isExplain = (serializationMode == SortKeySerialization::kForExplain);
                    keyObj[computedFieldName] = _sortPattern[i].expression->serialize(isExplain);
                    break;
                }
                case SortKeySerialization::kForSortKeyMerging: {
                    // We need to be able to tell which direction the sort is. Expression sorts are
                    // always descending.
                    keyObj[computedFieldName] = Value(-1);
                    break;
                }
            }
        }
    }
    return keyObj.freeze();
}

Pipeline::SourceContainer::iterator DocumentSourceSort::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    using SumType = long long;
    // Just in case, since we're passing an address of a signed 64bit below.
    static_assert(std::is_signed<SumType>::value && sizeof(SumType) == 8);

    auto stageItr = std::next(itr);
    SumType skipSum = 0;
    while (stageItr != container->end()) {
        auto nextStage = (*stageItr).get();
        auto nextSkip = dynamic_cast<DocumentSourceSkip*>(nextStage);
        auto nextLimit = dynamic_cast<DocumentSourceLimit*>(nextStage);
        SumType safeSum = 0;

        // The skip and limit values can be very large, so we need to make sure the sum doesn't
        // overflow before applying an optimiztion to pull the limit into the sort stage.
        if (nextSkip && !mongoSignedAddOverflow64(skipSum, nextSkip->getSkip(), &safeSum)) {
            skipSum = safeSum;
            ++stageItr;
        } else if (nextLimit &&
                   !mongoSignedAddOverflow64(nextLimit->getLimit(), skipSum, &safeSum)) {
            nextLimit->setLimit(safeSum);
            setLimitSrc(nextLimit);
            container->erase(stageItr);
            stageItr = std::next(itr);
            skipSum = 0;
        } else if (!nextStage->constraints().canSwapWithLimitAndSample) {
            return std::next(itr);
        } else {
            ++stageItr;
        }
    }

    return std::next(itr);
}

DepsTracker::State DocumentSourceSort::getDependencies(DepsTracker* deps) const {
    for (auto&& keyPart : _sortPattern) {
        if (keyPart.expression) {
            keyPart.expression->addDependencies(deps);
        } else {
            deps->fields.insert(keyPart.fieldPath->fullPath());
        }
    }
    if (pExpCtx->needsMerge) {
        // Include the sort key if we will merge several sorted streams later.
        deps->setNeedsMetadata(DepsTracker::MetadataType::SORT_KEY, true);
    }

    return DepsTracker::State::SEE_NEXT;
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
    boost::optional<uint64_t> maxMemoryUsageBytes) {
    intrusive_ptr<DocumentSourceSort> pSort(new DocumentSourceSort(pExpCtx));
    pSort->_maxMemoryUsageBytes = maxMemoryUsageBytes
        ? *maxMemoryUsageBytes
        : internalDocumentSourceSortMaxBlockingSortBytes.load();
    pSort->_rawSort = sortOrder.getOwned();

    for (auto&& keyField : sortOrder) {
        auto fieldName = keyField.fieldNameStringData();

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

    pSort->_sortKeyGen = SortKeyGenerator{
        // The SortKeyGenerator expects the expressions to be serialized in order to detect a sort
        // by a metadata field.
        pSort->sortKeyPattern(SortKeySerialization::kForPipelineSerialization).toBson(),
        pExpCtx->getCollator()};

    if (limit > 0) {
        pSort->setLimitSrc(DocumentSourceLimit::create(pExpCtx, limit));
    }

    return pSort;
}

SortOptions DocumentSourceSort::makeSortOptions() const {
    /* make sure we've got a sort key */
    verify(_sortPattern.size());

    SortOptions opts;
    if (_limitSrc)
        opts.limit = _limitSrc->getLimit();

    opts.maxMemoryUsageBytes = _maxMemoryUsageBytes;
    if (pExpCtx->allowDiskUse && !pExpCtx->inMongos) {
        opts.extSortAllowed = true;
        opts.tempDir = pExpCtx->tempDir;
    }

    return opts;
}

DocumentSource::GetNextResult DocumentSourceSort::populate() {
    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        loadDocument(nextInput.releaseDocument());
    }
    if (nextInput.isEOF()) {
        loadingDone();
    }
    return nextInput;
}

void DocumentSourceSort::loadDocument(Document&& doc) {
    invariant(!_populated);
    if (!_sorter) {
        _sorter.reset(MySorter::make(makeSortOptions(), Comparator(*this)));
    }

    Value sortKey;
    Document docForSorter;
    // We always need to extract the sort key if we've reached this point. If the query system had
    // already computed the sort key we'd have split the pipeline there, would be merging presorted
    // documents, and wouldn't use this method.
    std::tie(sortKey, docForSorter) = extractSortKey(std::move(doc));
    _sorter->add(sortKey, docForSorter);
}

void DocumentSourceSort::loadingDone() {
    if (!_sorter) {
        _sorter.reset(MySorter::make(makeSortOptions(), Comparator(*this)));
    }
    _output.reset(_sorter->done());
    _usedDisk = _sorter->usedDisk() || _usedDisk;
    _sorter.reset();
    _populated = true;
}

bool DocumentSourceSort::usedDisk() {
    return _usedDisk;
}

Value DocumentSourceSort::getCollationComparisonKey(const Value& val) const {
    const auto collator = pExpCtx->getCollator();

    // If the collation is the simple collation, the value itself is the comparison key.
    if (!collator) {
        return val;
    }

    // If 'val' is not a collatable type, there's no need to do any work.
    if (!CollationIndexKey::isCollatableType(val.getType())) {
        return val;
    }

    // If 'val' is a string, directly use the collator to obtain a comparison key.
    if (val.getType() == BSONType::String) {
        auto compKey = collator->getComparisonKey(val.getString());
        return Value(compKey.getKeyData());
    }

    // Otherwise, for non-string collatable types, take the slow path and round-trip the value
    // through BSON.
    BSONObjBuilder input;
    val.addToBsonObj(&input, ""_sd);

    BSONObjBuilder output;
    CollationIndexKey::collationAwareIndexKeyAppend(input.obj().firstElement(), collator, &output);
    return Value(output.obj().firstElement());
}

StatusWith<Value> DocumentSourceSort::extractKeyPart(const Document& doc,
                                                     const SortPatternPart& patternPart) const {
    Value plainKey;
    if (patternPart.fieldPath) {
        invariant(!patternPart.expression);
        auto key =
            document_path_support::extractElementAlongNonArrayPath(doc, *patternPart.fieldPath);
        if (!key.isOK()) {
            return key;
        }
        plainKey = key.getValue();
    } else {
        invariant(patternPart.expression);
        plainKey = patternPart.expression->evaluate(doc);
    }

    return getCollationComparisonKey(plainKey);
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

BSONObj DocumentSourceSort::extractKeyWithArray(const Document& doc) const {
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
    return uassertStatusOK(_sortKeyGen->getSortKey(std::move(bsonDoc), &metadata));
}

std::pair<Value, Document> DocumentSourceSort::extractSortKey(Document&& doc) const {
    boost::optional<BSONObj> serializedSortKey;  // Only populated if we need to merge with other
                                                 // sorted results later. Serialized in the standard
                                                 // BSON sort key format with empty field names,
                                                 // e.g. {'': 1, '': [2, 3]}.

    Value inMemorySortKey;  // The Value we will use for comparisons within the sorter.

    auto fastKey = extractKeyFast(doc);
    if (fastKey.isOK()) {
        inMemorySortKey = std::move(fastKey.getValue());
        if (pExpCtx->needsMerge) {
            serializedSortKey = serializeSortKey(_sortPattern.size(), inMemorySortKey);
        }
    } else {
        // We have to do it the slow way - through the sort key generator. This will generate a BSON
        // sort key, which is an object with empty field names. We then need to convert this BSON
        // representation into the corresponding array of keys as a Value. BSONObj {'': 1, '': [2,
        // 3]} becomes Value [1, [2, 3]].
        serializedSortKey = extractKeyWithArray(doc);
        inMemorySortKey = deserializeSortKey(_sortPattern.size(), *serializedSortKey);
    }

    MutableDocument toBeSorted(std::move(doc));
    if (pExpCtx->needsMerge) {
        // We need to be merged, so will have to be serialized. Save the sort key here to avoid
        // re-computing it during the merge.
        invariant(serializedSortKey);
        toBeSorted.setSortKeyMetaField(*serializedSortKey);
    }
    return {inMemorySortKey, toBeSorted.freeze()};
}

int DocumentSourceSort::compare(const Value& lhs, const Value& rhs) const {
    // DocumentSourceSort::populate() has already guaranteed that the sort key is non-empty.
    // However, the tricky part is deciding what to do if none of the sort keys are present. In that
    // case, consider the document "less".
    //
    // Note that 'comparator' must use binary comparisons here, as both 'lhs' and 'rhs' are
    // collation comparison keys.
    ValueComparator comparator;
    const size_t n = _sortPattern.size();
    if (n == 1) {  // simple fast case
        if (_sortPattern[0].isAscending)
            return comparator.compare(lhs, rhs);
        else
            return -comparator.compare(lhs, rhs);
    }

    // compound sort
    for (size_t i = 0; i < n; i++) {
        int cmp = comparator.compare(lhs[i], rhs[i]);
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

boost::optional<DocumentSource::MergingLogic> DocumentSourceSort::mergingLogic() {
    MergingLogic split;
    split.shardsStage = this;
    split.inputSortPattern = sortKeyPattern(SortKeySerialization::kForSortKeyMerging).toBson();
    if (_limitSrc) {
        split.mergingStage = DocumentSourceLimit::create(pExpCtx, _limitSrc->getLimit());
    }
    return split;
}

bool DocumentSourceSort::canRunInParallelBeforeOut(
    const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) const {
    // This is an interesting special case. If there are no further stages which require merging the
    // streams into one, a $sort should not require it. This is only the case because the sort order
    // doesn't matter for a pipeline ending with a $out stage. We may encounter it here as an
    // intermediate stage before a final $group with a $sort, which would make sense. Should we
    // extend our analysis to detect if an exchange is appropriate in a general pipeline, a $sort
    // would generally require merging the streams before producing output.
    return false;
}

}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
// Explicit instantiation unneeded since we aren't exposing Sorter outside of this file.
