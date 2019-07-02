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

}  // namespace

constexpr StringData DocumentSourceSort::kStageName;

DocumentSourceSort::DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       const BSONObj& sortOrder,
                                       uint64_t limit,
                                       uint64_t maxMemoryUsageBytes)
    : DocumentSource(pExpCtx),
      _sortExecutor({{sortOrder, pExpCtx},
                     limit,
                     maxMemoryUsageBytes,
                     pExpCtx->tempDir,
                     pExpCtx->allowDiskUse}),
      // The SortKeyGenerator expects the expressions to be serialized in order to detect a sort
      // by a metadata field.
      _sortKeyGen({sortOrder, pExpCtx->getCollator()}) {
    uassert(15976,
            "$sort stage must have at least one sort key",
            !_sortExecutor->sortPattern().empty());
}

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

    auto result = _sortExecutor->getNext();
    if (!result)
        return GetNextResult::makeEOF();
    return GetNextResult(std::move(*result));
}

void DocumentSourceSort::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    uint64_t limit = _sortExecutor->getLimit();
    if (explain) {  // always one Value for combined $sort + $limit
        array.push_back(Value(DOC(
            kStageName << DOC("sortKey"
                              << _sortExecutor->sortPattern().serialize(
                                     SortPattern::SortKeySerialization::kForExplain)
                              << "limit"
                              << (_sortExecutor->hasLimit() ? Value(static_cast<long long>(limit))
                                                            : Value())))));
    } else {  // one Value for $sort and maybe a Value for $limit
        MutableDocument inner(_sortExecutor->sortPattern().serialize(
            SortPattern::SortKeySerialization::kForPipelineSerialization));
        array.push_back(Value(DOC(kStageName << inner.freeze())));

        if (_sortExecutor->hasLimit()) {
            auto limitSrc = DocumentSourceLimit::create(pExpCtx, limit);
            limitSrc->serializeToArray(array);
        }
    }
}

long long DocumentSourceSort::getLimit() const {
    return _sortExecutor->hasLimit() ? _sortExecutor->getLimit() : -1;
}

Pipeline::SourceContainer::iterator DocumentSourceSort::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto stageItr = std::next(itr);
    int64_t skipSum = 0;
    while (stageItr != container->end()) {
        auto nextStage = (*stageItr).get();
        auto nextSkip = dynamic_cast<DocumentSourceSkip*>(nextStage);
        auto nextLimit = dynamic_cast<DocumentSourceLimit*>(nextStage);
        int64_t safeSum = 0;

        // The skip and limit values can be very large, so we need to make sure the sum doesn't
        // overflow before applying an optimization to pull the limit into the sort stage.
        if (nextSkip && !mongoSignedAddOverflow64(skipSum, nextSkip->getSkip(), &safeSum)) {
            skipSum = safeSum;
            ++stageItr;
        } else if (nextLimit &&
                   !mongoSignedAddOverflow64(nextLimit->getLimit(), skipSum, &safeSum)) {
            _sortExecutor->setLimit(safeSum);
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
    for (auto&& keyPart : _sortExecutor->sortPattern()) {
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
    uint64_t limit,
    boost::optional<uint64_t> maxMemoryUsageBytes) {
    auto resolvedMaxBytes = maxMemoryUsageBytes
        ? *maxMemoryUsageBytes
        : internalDocumentSourceSortMaxBlockingSortBytes.load();
    intrusive_ptr<DocumentSourceSort> pSort(
        new DocumentSourceSort(pExpCtx, sortOrder.getOwned(), limit, resolvedMaxBytes));

    return pSort;
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

    Value sortKey;
    Document docForSorter;
    // We always need to extract the sort key if we've reached this point. If the query system had
    // already computed the sort key we'd have split the pipeline there, would be merging presorted
    // documents, and wouldn't use this method.
    std::tie(sortKey, docForSorter) = extractSortKey(std::move(doc));
    _sortExecutor->add(sortKey, docForSorter);
}

void DocumentSourceSort::loadingDone() {
    _sortExecutor->loadingDone();
    _populated = true;
}

bool DocumentSourceSort::usedDisk() {
    return _sortExecutor->wasDiskUsed();
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

StatusWith<Value> DocumentSourceSort::extractKeyPart(
    const Document& doc, const SortPattern::SortPatternPart& patternPart) const {
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
        plainKey = patternPart.expression->evaluate(doc, &pExpCtx->variables);
    }

    return getCollationComparisonKey(plainKey);
}

StatusWith<Value> DocumentSourceSort::extractKeyFast(const Document& doc) const {
    if (_sortExecutor->sortPattern().size() == 1u) {
        return extractKeyPart(doc, _sortExecutor->sortPattern()[0]);
    }

    vector<Value> keys;
    keys.reserve(_sortExecutor->sortPattern().size());
    for (auto&& keyPart : _sortExecutor->sortPattern()) {
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
    auto bsonDoc = _sortExecutor->sortPattern().documentToBsonWithSortPaths(doc);
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
            serializedSortKey =
                serializeSortKey(_sortExecutor->sortPattern().size(), inMemorySortKey);
        }
    } else {
        // We have to do it the slow way - through the sort key generator. This will generate a BSON
        // sort key, which is an object with empty field names. We then need to convert this BSON
        // representation into the corresponding array of keys as a Value. BSONObj {'': 1, '': [2,
        // 3]} becomes Value [1, [2, 3]].
        serializedSortKey = extractKeyWithArray(doc);
        inMemorySortKey =
            deserializeSortKey(_sortExecutor->sortPattern().size(), *serializedSortKey);
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

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSort::distributedPlanLogic() {
    DistributedPlanLogic split;
    split.shardsStage = this;
    split.inputSortPattern = _sortExecutor->sortPattern()
                                 .serialize(SortPattern::SortKeySerialization::kForSortKeyMerging)
                                 .toBson();
    if (_sortExecutor->hasLimit()) {
        split.mergingStage = DocumentSourceLimit::create(pExpCtx, getLimit());
    }
    return split;
}

bool DocumentSourceSort::canRunInParallelBeforeWriteStage(
    const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) const {
    // This is an interesting special case. If there are no further stages which require merging the
    // streams into one, a $sort should not require it. This is only the case because the sort order
    // doesn't matter for a pipeline ending with a write stage. We may encounter it here as an
    // intermediate stage before a final $group with a $sort, which would make sense. Should we
    // extend our analysis to detect if an exchange is appropriate in a general pipeline, a $sort
    // would generally require merging the streams before producing output.
    return false;
}

}  // namespace mongo
