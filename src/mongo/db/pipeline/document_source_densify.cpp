/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_densify.h"

#include "mongo/db/pipeline/expression.h"
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <iterator>
#include <memory>
#include <tuple>

#include "mongo/base/error_codes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/pipeline/document_source_fill.h"
#include "mongo/db/pipeline/document_source_fill_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using SortPatternPart = mongo::SortPattern::SortPatternPart;
using ExplicitBounds = mongo::RangeStatement::ExplicitBounds;
using Full = mongo::RangeStatement::Full;
using Partition = mongo::RangeStatement::Partition;
using DensifyValue = mongo::DensifyValue;

namespace mongo {

RangeStatement RangeStatement::parse(RangeSpec spec) {
    Value step = spec.getStep();
    ValueComparator comp = ValueComparator();
    uassert(5733401,
            "The step parameter in a range statement must be a strictly positive numeric value",
            step.numeric() && comp.evaluate(step > Value(0)));

    optional<TimeUnit> unit = [&]() {
        if (auto unit = spec.getUnit()) {
            uassert(6586400,
                    "The step parameter in a range statement must be a whole number when "
                    "densifying a date range",
                    step.integral64Bit());
            return optional<TimeUnit>(parseTimeUnit(unit.value()));
        } else {
            return optional<TimeUnit>(boost::none);
        }
    }();

    Bounds bounds = [&]() {
        BSONElement bounds = spec.getBounds().getElement();
        switch (bounds.type()) {
            case mongo::Array: {
                std::vector<BSONElement> array = bounds.Array();

                uassert(5733403,
                        "A bounding array in a range statement must have exactly two elements",
                        array.size() == 2);
                uassert(5733402,
                        "A bounding array must be an ascending array of either two dates or two "
                        "numbers",
                        comp.evaluate(Value(array[0]) <= Value(array[1])));
                if (array[0].isNumber()) {
                    uassert(5733409, "Numeric bounds may not have unit parameter", !unit);
                    uassert(5733406,
                            "A bounding array must contain either both dates or both numeric types",
                            array[1].isNumber());
                    // If these values are types of different sizes, output type may not be
                    // intuitive.
                    uassert(5876900,
                            "Upper bound, lower bound, and step must all have the same type",
                            array[0].type() == array[1].type() &&
                                array[0].type() == step.getType());
                    return Bounds(std::pair<Value, Value>(Value(array[0]), Value(array[1])));
                } else if (array[0].type() == mongo::Date) {
                    uassert(5733405,
                            "A bounding array must contain either both dates or both numeric types",
                            array[1].type() == mongo::Date);
                    uassert(5733410, "A bounding array of dates must specify a unit", unit);
                    return Bounds(std::pair<Date_t, Date_t>(array[0].date(), array[1].date()));
                } else {
                    uasserted(5946800, "Explicit bounds must be numeric or dates");
                }
                MONGO_UNREACHABLE_TASSERT(5946801);
            }
            case mongo::String: {
                if (bounds.str() == kValFull)
                    return Bounds(Full());
                else if (bounds.str() == kValPartition)
                    return Bounds(Partition());
                else
                    uasserted(5946802,
                              str::stream() << "Bounds string must either be '" << kValFull
                                            << "' or '" << kValPartition << "'");
                MONGO_UNREACHABLE_TASSERT(5946803);
            }
            default:
                uasserted(5733404,
                          "The bounds in a range statement must be the string \'full\', "
                          "\'partition\', or an ascending array of two numbers or two dates");
        }
    }();

    RangeStatement range = RangeStatement(step, bounds, unit);
    return range;
}

REGISTER_DOCUMENT_SOURCE(densify,
                         LiteParsedDocumentSourceDefault::parse,
                         document_source_densify::createFromBson,
                         AllowedWithApiStrict::kAlways);

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalDensify,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceInternalDensify::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalDensify, DocumentSourceInternalDensify::id)

namespace document_source_densify {

list<intrusive_ptr<DocumentSource>> createFromBsonInternal(
    BSONElement elem,
    const intrusive_ptr<ExpressionContext>& expCtx,
    StringData stageName,
    bool isInternal) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << stageName << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec = DensifySpec::parse(IDLParserContext(stageName), elem.embeddedObject());
    auto rangeStatement = RangeStatement::parse(spec.getRange());

    list<FieldPath> partitions;
    if (spec.getPartitionByFields()) {
        auto partitionFields = (*spec.getPartitionByFields());
        for (auto& partitionField : partitionFields) {
            // The densified field cannot be included in a partition field.
            uassert(8993000,
                    fmt::format("{} '{}' cannot include {} '{}' that is being densified.",
                                DocumentSourceInternalDensify::kPartitionByFieldsFieldName,
                                partitionField,
                                DocumentSourceInternalDensify::kFieldFieldName,
                                spec.getField()),
                    !partitionField.starts_with(spec.getField()));

            // A partition field cannot be included in the densified field.
            uassert(9554500,
                    fmt::format("{} '{}' that is being densified cannot include {} '{}'.",
                                DocumentSourceInternalDensify::kFieldFieldName,
                                spec.getField(),
                                DocumentSourceInternalDensify::kPartitionByFieldsFieldName,
                                partitionField),
                    !spec.getField().starts_with(partitionField));
            partitions.push_back(FieldPath(partitionField));
        }
    }

    FieldPath field = FieldPath(spec.getField());

    if (holds_alternative<RangeStatement::Partition>(rangeStatement.getBounds()) &&
        partitions.empty())
        uasserted(5733408,
                  "One cannot specify the bounds as 'partition' without specifying a non-empty "
                  "array of partitionByFields. You may have meant to specify 'full' bounds.");

    auto densifyStage = create(
        expCtx, std::move(partitions), std::move(field), std::move(rangeStatement), isInternal);
    return densifyStage;
}

list<intrusive_ptr<DocumentSource>> createFromBson(BSONElement elem,
                                                   const intrusive_ptr<ExpressionContext>& expCtx) {
    return createFromBsonInternal(elem, expCtx, kStageName, false);
}

SortPattern getSortPatternForDensify(RangeStatement rangeStatement,
                                     list<FieldPath> partitions,
                                     FieldPath field) {
    // Add partition fields to sort spec.
    std::vector<SortPatternPart> sortParts;
    // We do not add partitions to the sort spec if the range is "full".
    if (!holds_alternative<Full>(rangeStatement.getBounds())) {
        for (const auto& partition : partitions) {
            SortPatternPart part;
            part.fieldPath = partition.fullPath();
            sortParts.push_back(std::move(part));
        }
    }

    // Add field path to sort spec if it is not yet in the sort spec.
    const auto inserted = std::find_if(
        sortParts.begin(), sortParts.end(), [&field](const SortPatternPart& s) -> bool {
            return s.fieldPath->fullPath().compare(field.fullPath()) == 0;
        });
    if (inserted == sortParts.end()) {
        SortPatternPart part;
        part.fieldPath = field.fullPath();
        sortParts.push_back(std::move(part));
    }
    return SortPattern{std::move(sortParts)};
}

list<intrusive_ptr<DocumentSource>> create(const intrusive_ptr<ExpressionContext>& expCtx,
                                           list<FieldPath> partitions,
                                           FieldPath field,
                                           RangeStatement rangeStatement,
                                           bool isInternal) {
    list<intrusive_ptr<DocumentSource>> results;

    // If we're creating an internal stage then we must not desugar and produce a sort stage in
    // addition.
    if (!isInternal) {
        auto sortPattern = getSortPatternForDensify(rangeStatement, partitions, field);
        // Constructing resulting stages.
        results.push_back(DocumentSourceSort::create(expCtx, sortPattern));
    }

    // Constructing resulting stages.
    results.push_back(make_intrusive<DocumentSourceInternalDensify>(
        expCtx, std::move(field), std::move(partitions), std::move(rangeStatement)));

    return results;
}
}  // namespace document_source_densify

// Create an object to be passed to a generator containing fields that must be propagated to
// generated documents.
boost::optional<Document> DocumentSourceInternalDensify::createIncludeFieldsObj(
    Value partitionKey) {
    if (_partitionExpr->selfAndChildrenAreConstant()) {
        return boost::none;
    }
    return boost::make_optional<Document>(partitionKey.getDocument());
}
DocumentSourceInternalDensify::DocGenerator::DocGenerator(DensifyValue min,
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

Document DocumentSourceInternalDensify::DocGenerator::getNextDocument() {
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

bool DocumentSourceInternalDensify::DocGenerator::done() const {
    return _state == GeneratorState::kDone;
}

bool DocumentSourceInternalDensify::DocGenerator::lastGeneratedDoc() const {
    return _state == GeneratorState::kReturningFinalDocument ||
        (_state == GeneratorState::kDone && _finalDoc == boost::none);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalDensify::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto results = document_source_densify::createFromBsonInternal(elem, expCtx, kStageName, true);
    tassert(5733413,
            "When creating an $_internalDensify stage, only one stage should be returned",
            results.size() == 1);
    return results.front();
}

Value DocumentSourceInternalDensify::serialize(const SerializationOptions& opts) const {
    MutableDocument spec;
    spec[kFieldFieldName] = Value(opts.serializeFieldPath(_field));
    std::vector<Value> serializedPartitionByFields(_partitions.size());
    std::transform(_partitions.begin(),
                   _partitions.end(),
                   serializedPartitionByFields.begin(),
                   [&](FieldPath field) -> Value { return Value(opts.serializeFieldPath(field)); });
    spec[kPartitionByFieldsFieldName] = Value(std::move(serializedPartitionByFields));
    spec[kRangeFieldName] = _range.serialize(opts);
    MutableDocument out;
    out[getSourceName()] = Value(spec.freeze());

    return Value(out.freezeToValue());
}

// Return true if the second return value should be returned without processing.
std::tuple<bool, DocumentSource::GetNextResult, DensifyValue, Value>
DocumentSourceInternalDensify::getAndCheckInvalidDoc() {
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

DocumentSource::GetNextResult DocumentSourceInternalDensify::finishDensifyingPartitionedInputHelper(
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
    return DocumentSource::GetNextResult::makeEOF();
}
DocumentSource::GetNextResult DocumentSourceInternalDensify::handleSourceExhausted() {
    // If the partition map is empty, we're done.
    if (_partitionTable.size() == 0) {
        _densifyState = DensifyState::kDensifyDone;
        return DocumentSource::GetNextResult::makeEOF();
    }
    // The "relevant" max is either:
    // 1. _fullDensifyGlobalMax if we are densifying with option 'full'
    // 2. _rangeDensifyEnd if we are densifying with option 'range'
    if (_isFullDensify) {
        // 'Full case'. If we haven't seen any documents yet, we can't generate more.
        if (!_fullDensifyGlobalMax && !_fullDensifyGlobalMin) {
            if (_partitionTable.size() == 0) {
                return DocumentSource::GetNextResult::makeEOF();
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
        return DocumentSource::GetNextResult::makeEOF();
    }
}

void DocumentSourceInternalDensify::initializeState() {
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

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGen(Document currentDoc,
                                                                           DensifyValue lastSeen,
                                                                           DensifyValue& densifyVal,
                                                                           Value& partitionKey) {
    auto nextValToGenerate = lastSeen.increment(_range);

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

DocumentSource::GetNextResult DocumentSourceInternalDensify::checkFirstDocAgainstRangeStart(
    Document doc, DensifyValue& densifyVal, Value& partitionKey) {
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

DocumentSource::GetNextResult DocumentSourceInternalDensify::doGetNext() {
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
                return DocumentSource::GetNextResult(std::move(doc));
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
            return DocumentSource::GetNextResult::makeEOF();
    }
    MONGO_UNREACHABLE_TASSERT(8246105);
}

DensifyValue DensifyValue::increment(const RangeStatement& range) const {
    return visit(OverloadedVisitor{[&](Value val) {
                                       return DensifyValue(uassertStatusOK(
                                           exec::expression::evaluateAdd(val, range.getStep())));
                                   },
                                   [&](Date_t date) {
                                       return DensifyValue(dateAdd(date,
                                                                   range.getUnit().value(),
                                                                   range.getStep().coerceToLong(),
                                                                   timezone()));
                                   }},
                 _value);
}

DensifyValue DensifyValue::decrement(const RangeStatement& range) const {
    return visit(
        OverloadedVisitor{
            [&](Value val) {
                return DensifyValue(
                    uassertStatusOK(exec::expression::evaluateSubtract(val, range.getStep())));
            },
            [&](Date_t date) {
                return DensifyValue(dateAdd(
                    date, range.getUnit().value(), -range.getStep().coerceToLong(), timezone()));
            }},
        _value);
}

Pipeline::SourceContainer::iterator DocumentSourceInternalDensify::combineSorts(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    if (std::next(itr) == container->end() || itr == container->begin()) {
        return container->end();
    }

    // We can only combine the sorts if we can guarantee the output order will maintain the
    // sort. Densify changes the sort order if partitions are present and range is type 'full'.
    if (_partitions.size() != 0 && holds_alternative<Full>(_range.getBounds())) {
        // We will not maintain sort order.
        return std::next(itr);
    }

    // If $densify was the first stage in the pipeline, there should be a preceding sort.
    tassert(6059802, "$_internalDensify did not have a preceding stage", itr != container->begin());
    // Get the spec of the preceding sort stage. Densify always has a preceding sort, unless
    // the preceding sort was already removed by an earlier stage.
    const auto preSortItr = std::prev(itr);
    const auto preSortStage = dynamic_cast<DocumentSourceSort*>((*preSortItr).get());
    if (!preSortStage || preSortStage->getLimit()) {
        return std::next(itr);
    }

    // Check that the preceding sort was actually generated by $densify, and not by combining the
    // generated sort with a sort earlier in the pipeline.
    auto densifySortPattern =
        document_source_densify::getSortPatternForDensify(_range, _partitions, _field);

    auto preDensifySortPattern = preSortStage->getSortKeyPattern();
    if (densifySortPattern != preDensifySortPattern) {
        return std::next(itr);
    }

    // Get the spec of the following sort stage, if it exists.
    const auto postSortItr = std::next(itr);
    const auto postSortStage = dynamic_cast<DocumentSourceSort*>((*postSortItr).get());
    if (!postSortStage || postSortStage->getLimit()) {
        // If there is not a following sort stage, we won't do any optimization. Return the next
        // stage in the pipeline.
        return std::next(itr);
    }
    auto postDensifySortPattern = postSortStage->getSortKeyPattern();

    // We can only combine the sorts if the sorts are compatible. $densify only preserves a sort on
    // the fields on which it operates, as any other fields will be missing in generated documents.
    if (!preDensifySortPattern.isExtensionOf(postDensifySortPattern)) {
        return std::next(itr);
    }

    // If the post sort is longer, we would have bailed earlier. Remove the sort after the $densify.
    container->erase(postSortItr);

    return std::prev(itr);
}

Pipeline::SourceContainer::iterator DocumentSourceInternalDensify::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    tassert(6059800, "Expected to optimize $densify stage", *itr == this);

    return combineSorts(itr, container);
}
}  // namespace mongo
