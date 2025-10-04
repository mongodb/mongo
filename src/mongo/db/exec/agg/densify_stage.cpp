/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/exec/agg/densify_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalDensifyToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto dsInternalDensify =
        boost::dynamic_pointer_cast<const DocumentSourceInternalDensify>(documentSource);

    tassert(10423800, "expected 'DocumentSourceInternalDensify' type", dsInternalDensify);

    return make_intrusive<exec::agg::InternalDensifyStage>(dsInternalDensify->kStageName,
                                                           dsInternalDensify->getExpCtx(),
                                                           dsInternalDensify->_field,
                                                           dsInternalDensify->_partitions,
                                                           dsInternalDensify->_range);
}

namespace exec {
namespace agg {

using Full = mongo::RangeStatement::Full;
using Partition = mongo::RangeStatement::Partition;
using ExplicitBounds = mongo::RangeStatement::ExplicitBounds;

REGISTER_AGG_STAGE_MAPPING(internalDensifyStage,
                           DocumentSourceInternalDensify::id,
                           documentSourceInternalDensifyToStageFn);

InternalDensifyStage::DocGenerator::DocGenerator(DensifyValue min,
                                                 RangeStatement range,
                                                 FieldPath fieldName,
                                                 boost::optional<Document> includeFields,
                                                 boost::optional<Document> finalDoc,
                                                 boost::optional<Value> partitionKey,
                                                 ValueComparator comp,
                                                 size_t* counter,
                                                 bool maxInclusive)
    : _comp(std::move(comp)),
      _range(std::move(range)),
      _path(std::move(fieldName)),
      _finalDoc(std::move(finalDoc)),
      _partitionKey(std::move(partitionKey)),
      _min(std::move(min)),
      _maxInclusive(maxInclusive),
      _counter(counter) {

    if (includeFields) {
        _includeFields = *includeFields;
        tassert(5733306,
                "DocGenerator cannot include field that is being densified",
                _includeFields.getNestedField(_path).missing());
    }

    // Traverse the preserved fields document to make sure we're not going through an array.
    auto traverseDoc = _includeFields;
    auto pathLength = _path.getPathLength();
    for (size_t i = 0; i < pathLength; ++i) {
        auto curVal = traverseDoc.getField(_path.getFieldName(i));
        uassert(5733307, "$densify cannot generate fields nested inside arrays", !curVal.isArray());
        if (curVal.isObject()) {
            traverseDoc = curVal.getDocument();
        } else {
            // Can't write to a field that has a non-object value as a path-prefix, as that would
            // overwrite data. We should only have a non-object at the end of the path.
            uassert(5733308,
                    "$densify cannot overwrite non-object values with objects",
                    i == pathLength - 1 || curVal.missing());
            break;
        }
    }

    tassert(
        5733305, "DocGenerator step must be positive", _comp.evaluate(_range.getStep() > Value(0)));

    tassert(5733700,
            "DocGenerator must be passed a range with ExplicitBounds",
            holds_alternative<ExplicitBounds>(_range.getBounds()));
    ExplicitBounds bounds = get<ExplicitBounds>(_range.getBounds());
    tassert(5733304,
            "DocGenerator all values must be same type",
            bounds.first.isSameTypeAs(bounds.second) && bounds.first.isSameTypeAs(_min));

    if (_min.isDate()) {
        // Extra checks for date step + unit.
        tassert(5733501, "Unit must be specified with a date step", _range.getUnit());
        tassert(5733505,
                "Step must be a whole number for date densification",
                _range.getStep().integral64Bit());
    } else {
        tassert(5733506, "Unit must not be specified with non-date values", !_range.getUnit());
    }

    tassert(5733303, "DocGenerator min must be lower or equal to max", _min <= bounds.second);
}

GetNextResult InternalDensifyStage::doGetNext() {
    uassert(5897900,
            str::stream() << "Generated " << _docsGenerated
                          << " documents in $densify, which is over the limit of " << _maxDocs
                          << ". Increase the 'internalQueryMaxAllowedDensifyDocs' parameter to "
                             "allow more generated documents",
            _docsGenerated <= _maxDocs);
    switch (_densifyState) {
        case DensifyState::kUninitialized: {
            // Initialize global vars (densifying min, densifying max).
            // Initialize partition state (even without partition, assume one partition).
            initializeState();
            auto [shouldReturnWithoutProcessing, origDoc, densifyVal, densifyPartition] =
                getAndCheckInvalidDoc();
            if (shouldReturnWithoutProcessing) {
                return origDoc;
            }
            auto doc = origDoc.getDocument();
            return checkFirstDocAgainstRangeStart(doc, densifyVal, densifyPartition);
        }
        case DensifyState::kNeedGen: {
            // Pull document
            auto [shouldReturnWithoutProcessing, origDoc, densifyVal, densifyPartition] =
                getAndCheckInvalidDoc();
            if (shouldReturnWithoutProcessing) {
                return origDoc;
            }
            auto doc = origDoc.getDocument();

            auto foundPartitionVal = _partitionTable.find(densifyPartition);
            // If we haven't seen this partition yet, no need for any action. We'll densify when we
            // see another document later, unless this is in the range.
            if (foundPartitionVal == _partitionTable.end()) {
                return checkFirstDocAgainstRangeStart(doc, densifyVal, densifyPartition);
            }
            auto lastPartitionVal = foundPartitionVal->second.value();
            if (_rangeDensifyEnd && lastPartitionVal >= _rangeDensifyEnd) {
                // If we've already finished densifying this partition, just return this document.
                return doc;
            }
            // Set the new place to start densifying to either the bottom of the explicit range or
            // the last thing seen in the partition.
            if (_rangeDensifyStart && _rangeDensifyStart > lastPartitionVal) {
                lastPartitionVal = _rangeDensifyStart->decrement(_range);
            }
            if (lastPartitionVal > densifyVal) {
                // This document is not yet in the densification range.
                return GetNextResult(std::move(doc));
            }
            return handleNeedGen(doc, lastPartitionVal, densifyVal, densifyPartition);
        }
        case DensifyState::kHaveGenerator: {
            // Pull document from generator.
            tassert(5733203,
                    "Densify state is kHaveGenerator but DocGenerator is null or done.",
                    _docGenerator && !_docGenerator->done());

            auto generatedDoc = _docGenerator->getNextDocument();
            // Only set the partition value if this document is the last on-step document that this
            // generator will produce. The last on-step document is the last generated document OR
            // the 'finalDoc' that was placed in the generator when it was built.
            if (_docGenerator->done() || _docGenerator->lastGeneratedDoc()) {
                auto genDensifyVal = getDensifyValue(generatedDoc);
                auto partition = getDensifyPartition(generatedDoc);
                // If we're densifying we must have seen a value in this partition already.
                auto foundPartitionVal = _partitionTable.find(partition);
                tassert(8246104,
                        "Expected value in the partition",
                        foundPartitionVal != _partitionTable.end());
                // Verify this is on the step. If the document was generated it automatically is,
                // otherwise it is the final doc and may happen to be on the step.
                if (_docGenerator->lastGeneratedDoc() ||
                    genDensifyVal == foundPartitionVal->second.value().increment(_range)) {
                    setPartitionValue(genDensifyVal, partition);
                }
            }
            // Update state.
            if (_docGenerator->done()) {
                _docGenerator = boost::none;
                _densifyState = DensifyState::kNeedGen;
                // We haven't seen EOF yet -- if we have, we would be in
                // kFinishingDensifyWithGenerator. Assume we're not done.
            }
            return generatedDoc;
        }
        case DensifyState::kFinishingDensifyWithGenerator: {
            // We don't have to update the table anymore, we're just finishing what we've already
            // seen.
            auto generatedDoc = _docGenerator->getNextDocument();
            if (_docGenerator->done()) {
                _docGenerator = boost::none;
                _densifyState = DensifyState::kFinishingDensifyNoGenerator;
            }
            return generatedDoc;
        }
        case DensifyState::kFinishingDensifyNoGenerator: {
            return handleSourceExhausted();
        }
        case DensifyState::kDensifyDone:
            return GetNextResult::makeEOF();
    }
    MONGO_UNREACHABLE_TASSERT(8246105);
}

GetNextResult InternalDensifyStage::finishDensifyingPartitionedInputHelper(
    DensifyValue max, boost::optional<DensifyValue> minOverride, bool maxInclusive) {
    // We remove a partition from the table once its done.
    while (_partitionTable.size() != 0) {
        auto firstPartitionKeyVal = _partitionTable.begin();
        Value firstPartition = firstPartitionKeyVal->first;
        DensifyValue firstPartitionVal = firstPartitionKeyVal->second.value();

        // We've already seen the stored value, we want to start generating on the next
        // one.
        auto valToGenerate = firstPartitionVal.increment(_range);
        // If the partition never hit the bottom of the range, use that instead.
        if (minOverride && minOverride > firstPartitionVal) {
            valToGenerate = *minOverride;
        }

        // If the valToGenerate is > max seen, skip this partition. It is done.
        if (valToGenerate > max || (!maxInclusive && valToGenerate >= max)) {
            _partitionTable.erase(firstPartitionKeyVal);
            continue;
        }

        createDocGenerator(
            valToGenerate,
            RangeStatement(_range.getStep(), ExplicitBounds(valToGenerate, max), _range.getUnit()),
            createIncludeFieldsObj(firstPartition),
            boost::none,     // finalDoc
            firstPartition,  // partitionKey
            maxInclusive);
        // Remove this partition from the table, we're done with it. Note that we may still have
        // documents to generate, but we won't ever need to process it again.
        _partitionTable.erase(firstPartitionKeyVal);
        auto returnDoc = _docGenerator->getNextDocument();
        if (_docGenerator->done()) {
            _docGenerator = boost::none;
            _densifyState = DensifyState::kFinishingDensifyNoGenerator;
        } else {
            _densifyState = DensifyState::kFinishingDensifyWithGenerator;
        }
        return returnDoc;
    }
    _densifyState = DensifyState::kDensifyDone;
    return GetNextResult::makeEOF();
}

GetNextResult InternalDensifyStage::handleSourceExhausted() {
    // If the partition map is empty, we're done.
    if (_partitionTable.size() == 0) {
        _densifyState = DensifyState::kDensifyDone;
        return GetNextResult::makeEOF();
    }
    // The "relevant" max is either:
    // 1. _fullDensifyGlobalMax if we are densifying with option 'full'
    // 2. _rangeDensifyEnd if we are densifying with option 'range'
    if (_isFullDensify) {
        // 'Full case'. If we haven't seen any documents yet, we can't generate more.
        if (!_fullDensifyGlobalMax && !_fullDensifyGlobalMin) {
            if (_partitionTable.size() == 0) {
                return GetNextResult::makeEOF();
            }
            tasserted(8246101, "Expected global min/max to be set for 'full' case");
        }
        // If we are in the partitioned case, and we saw a value in a partition, we need to create
        // that value in every other partition. This is the "fullDensifyGlobalMax" and unfortunately
        // needs to be considered inclusive, in contrast to every other case.
        return finishDensifyingPartitionedInputHelper(
            *_fullDensifyGlobalMax, *_fullDensifyGlobalMin, true);
    } else if (_rangeDensifyEnd) {
        // 'range' case. Use the user provided bounds.
        tassert(8246102,
                "Expected densify start/end to be set for 'range' case",
                _rangeDensifyEnd && _rangeDensifyStart);
        return finishDensifyingPartitionedInputHelper(
            *_rangeDensifyEnd, *_rangeDensifyStart, false);
    } else {
        // No work for 'partition' case. The max value in each partition is the last document
        // pulled from the collection.
        _densifyState = DensifyState::kDensifyDone;
        return GetNextResult::makeEOF();
    }
}

GetNextResult InternalDensifyStage::handleNeedGen(Document currentDoc,
                                                  DensifyValue lastSeen,
                                                  DensifyValue& densifyVal,
                                                  Value& partitionKey) {
    auto nextValToGenerate = lastSeen.increment(_range);

    // It is possible that when stepping by a unit of time, due to variance in the length of months
    // or years, when we initially decrement by our step we could end up on a different day than
    // expected (For example March 31 - 1 month would give us February 28). When we add the step
    // back, this could mean our new date is outside of the range (for a range starting on March
    // 31st, adding 1 month to February 28th would return March 28th, which is outside of the
    // range). This will only ever happen at the start of the range. Therefore, it is safe to assume
    // that if the value generated is below the range, the correct value is the start of the range.
    // TODO SERVER-99860: This solution may need to be overwritten depending on conclusions from
    // this ticket.
    if (_rangeDensifyStart && nextValToGenerate < _rangeDensifyStart) {
        nextValToGenerate = *_rangeDensifyStart;
    }

    // If we don't need to create a generator (no intervening documents to generate before
    // outputting currentDoc), then don't create a generator. Altenatively if this document is above
    // where we need to generated documents, also don't create a generator.
    if (densifyVal <= nextValToGenerate ||
        (_rangeDensifyEnd && nextValToGenerate >= _rangeDensifyEnd)) {
        // If the current value is the next value to be generated, save it as the last seen
        // value.
        if (densifyVal == nextValToGenerate) {
            setPartitionValue(densifyVal, partitionKey);
        }
        return currentDoc;
    }

    // Falling through the above conditions means the currentDoc is strictly greater than the last
    // seen document plus the step value.
    // Save the next value to be generated in the partition to note that we've seen a value in it.
    setPartitionValue(nextValToGenerate, partitionKey);
    // Don't generate past explicit bounds if present.
    auto maxRange =
        _rangeDensifyEnd && _rangeDensifyEnd <= densifyVal ? _rangeDensifyEnd : densifyVal;
    createDocGenerator(nextValToGenerate,
                       RangeStatement(_range.getStep(),
                                      ExplicitBounds(nextValToGenerate, *maxRange),
                                      _range.getUnit()),
                       createIncludeFieldsObj(partitionKey),
                       currentDoc,
                       partitionKey);

    _densifyState = DensifyState::kHaveGenerator;
    auto nextDoc = _docGenerator->getNextDocument();
    if (_docGenerator->done()) {
        _docGenerator = boost::none;
        _densifyState = DensifyState::kNeedGen;
    }

    return nextDoc;
}

GetNextResult InternalDensifyStage::checkFirstDocAgainstRangeStart(Document doc,
                                                                   DensifyValue& densifyVal,
                                                                   Value& partitionKey) {
    // If this document is in the range already, create a generator to densify. Otherwise return the
    // doc.
    if (_rangeDensifyStart && densifyVal > _rangeDensifyStart) {
        setPartitionValue(*_rangeDensifyStart, partitionKey);
        // Note that we don't use the doc here as the 'lastSeen' value. We always want to start at
        // the beginning of the range.
        return handleNeedGen(doc, _rangeDensifyStart->decrement(_range), densifyVal, partitionKey);
    } else if (_isFullDensify) {
        // This is the first value in the partition. We may have to create documents based on other
        // partitions in the full case.
        if (densifyVal > *_fullDensifyGlobalMin &&
            densifyVal > _fullDensifyGlobalMin->increment(_range)) {
            // This value is above where the next document would need to be generated.
            return handleNeedGen(
                doc, _fullDensifyGlobalMin->decrement(_range), densifyVal, partitionKey);
        } else if (densifyVal > *_fullDensifyGlobalMin) {
            // This value is between the minimum and the next step. Store that we've seen this
            // partition and we'll use the minimum value for it.
            setPartitionValue(_fullDensifyGlobalMin->decrement(_range), partitionKey);
            return doc;
        }
        // Else this document is equal to the next value. We don't need to generate a document, so
        // just save this value and return it.
    }
    setPartitionValue(densifyVal, partitionKey);
    return doc;
}

Document InternalDensifyStage::DocGenerator::getNextDocument() {
    tassert(5733301,
            "Called DocGenerator::getNextDocument() but generator is done",
            _state != GeneratorState::kDone);
    if (_state == GeneratorState::kReturningFinalDocument) {
        _state = GeneratorState::kDone;
        // If _finalDoc is boost::none we can't be in this state.
        tassert(5832800, "DocGenerator expected _finalDoc, found boost::none", _finalDoc);
        return _finalDoc.value();
    }
    // Assume all types have been checked at this point and we are in a valid state.
    DensifyValue valueToAdd = _min;
    DensifyValue nextValue = _min.increment(_range);
    ExplicitBounds bounds = get<ExplicitBounds>(_range.getBounds());

    if (bounds.second < nextValue || (!_maxInclusive && bounds.second <= nextValue)) {
        _state = _finalDoc ? GeneratorState::kReturningFinalDocument : GeneratorState::kDone;
    }
    _min = nextValue;

    MutableDocument retDoc(_includeFields);
    retDoc.setNestedField(_path, valueToAdd.toValue());
    ++(*_counter);
    return retDoc.freeze();
}

// Return true if the second return value should be returned without processing.
std::tuple<bool, GetNextResult, DensifyValue, Value> InternalDensifyStage::getAndCheckInvalidDoc() {
    auto nextDoc = pSource->getNext();
    if (!nextDoc.isAdvanced()) {
        if (nextDoc.isEOF()) {
            auto docToReturn = handleSourceExhausted();
            return std::make_tuple(true, docToReturn, DensifyValue(), Value());
        }
        return std::make_tuple(true, nextDoc, DensifyValue(), Value());
    }

    auto doc = nextDoc.getDocument();
    auto densifyField = doc.getNestedField(_field);
    if (densifyField.nullish()) {
        // The densify field is not present or null, let document pass unmodified.
        return std::make_tuple(true, doc, DensifyValue(), Value());
    }
    // We will be densifying. Set up state for all future functions.
    auto densifyVal = getDensifyValue(densifyField);
    auto partitionKey = getDensifyPartition(doc);
    // Track the global max for later. The latest from the source is always the global max.
    if (_isFullDensify) {
        _fullDensifyGlobalMax = densifyVal;
        if (!_fullDensifyGlobalMin) {
            // First value seen is the global min.
            _fullDensifyGlobalMin = _fullDensifyGlobalMax;
        }
    }
    return std::make_tuple(false, doc, densifyVal, partitionKey);
}

void InternalDensifyStage::initializeState() {
    if (_partitions.empty()) {
        // Initialize table to one row with true as the key. This allows us to treat all inputs as
        // partitioned to simplify the code.
        _partitionExpr = ExpressionConstant::create(pExpCtx.get(), Value(true));
    } else {
        // Otherwise create a partition expression we can use to generate partition keys.
        MutableDocument partitionExpr;
        for (auto&& p : _partitions) {
            partitionExpr.setNestedField(p.fullPath(), Value{"$"_sd + p.fullPath()});
        }
        _partitionExpr = ExpressionObject::parse(
            pExpCtx.get(), partitionExpr.freeze().toBson(), pExpCtx->variablesParseState);
    }
    visit(OverloadedVisitor{[&](Full) {
                                _isFullDensify = true;
                                _rangeDensifyStart = boost::none;
                                _rangeDensifyEnd = boost::none;
                            },
                            [&](Partition) {
                                _rangeDensifyStart = boost::none;
                                _rangeDensifyEnd = boost::none;
                            },
                            [&](ExplicitBounds bounds) {
                                _fullDensifyGlobalMax = boost::none;
                                _rangeDensifyStart = bounds.first;
                                // If we are not partitioned, we have to make sure we generate
                                // documents for the collection.
                                if (_partitions.empty()) {
                                    setPartitionValue(Document(),
                                                      _rangeDensifyStart->decrement(_range));
                                }
                                _rangeDensifyEnd = bounds.second;
                            }},
          _range.getBounds());
    _densifyState = DensifyState::kNeedGen;
}

// Create an object to be passed to a generator containing fields that must be propagated to
// generated documents.
boost::optional<Document> InternalDensifyStage::createIncludeFieldsObj(Value partitionKey) {
    if (_partitionExpr->selfAndChildrenAreConstant()) {
        return boost::none;
    }
    return boost::make_optional<Document>(partitionKey.getDocument());
}

bool InternalDensifyStage::DocGenerator::done() const {
    return _state == GeneratorState::kDone;
}

bool InternalDensifyStage::DocGenerator::lastGeneratedDoc() const {
    return _state == GeneratorState::kReturningFinalDocument ||
        (_state == GeneratorState::kDone && _finalDoc == boost::none);
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
