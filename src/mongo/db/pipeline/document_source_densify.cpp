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
#include "mongo/base/exact_cast.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

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
            return optional<TimeUnit>(parseTimeUnit(unit.get()));
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
        for (auto partitionField : partitionFields)
            partitions.push_back(FieldPath(partitionField));
    }

    FieldPath field = FieldPath(spec.getField());

    if (stdx::holds_alternative<RangeStatement::Partition>(rangeStatement.getBounds()) &&
        partitions.empty())
        uasserted(5733408,
                  "One cannot specify the bounds as 'partition' without specifying a non-empty "
                  "array of partitionByFields. You may have meant to specify 'full' bounds.");

    return create(std::move(expCtx),
                  std::move(partitions),
                  std::move(field),
                  std::move(rangeStatement),
                  isInternal);
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
    if (!stdx::holds_alternative<Full>(rangeStatement.getBounds())) {
        for (auto partition : partitions) {
            SortPatternPart part;
            part.fieldPath = partition.fullPath();
            sortParts.push_back(std::move(part));
        }
    }

    // Add field path to sort spec.
    SortPatternPart part;
    part.fieldPath = field.fullPath();
    sortParts.push_back(std::move(part));
    return SortPattern{sortParts};
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
    results.push_back(
        make_intrusive<DocumentSourceInternalDensify>(expCtx, field, partitions, rangeStatement));

    return results;
}
}  // namespace document_source_densify

DocumentSourceInternalDensify::DocGenerator::DocGenerator(DensifyValue min,
                                                          RangeStatement range,
                                                          FieldPath fieldName,
                                                          boost::optional<Document> includeFields,
                                                          boost::optional<Document> finalDoc,
                                                          ValueComparator comp,
                                                          size_t* counter)
    : _comp(std::move(comp)),
      _range(std::move(range)),
      _path(std::move(fieldName.fullPath())),
      _finalDoc(std::move(finalDoc)),
      _min(std::move(min)),
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
            stdx::holds_alternative<ExplicitBounds>(_range.getBounds()));
    ExplicitBounds bounds = stdx::get<ExplicitBounds>(_range.getBounds());
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
        return _finalDoc.get();
    }
    // Assume all types have been checked at this point and we are in a valid state.
    DensifyValue valueToAdd = _min;
    DensifyValue nextValue = _min.increment(_range);
    ExplicitBounds bounds = stdx::get<ExplicitBounds>(_range.getBounds());

    if (bounds.second <= nextValue) {
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

DocumentSource::GetNextResult DocumentSourceInternalDensify::densifyExplicitRangeAfterEOF() {
    tassert(5734403,
            "Expected explicit range in order to densify after last document.",
            stdx::holds_alternative<ExplicitBounds>(_range.getBounds()));
    auto bounds = stdx::get<ExplicitBounds>(_range.getBounds());
    // Once we have hit an EOF, if the last seen value (_current) plus the step is greater
    // than or equal to the rangeMax, that means we have finished densifying
    // over the explicit range so we just return an EOF. Otherwise, we finish
    // densifying over the rest of the range.
    if (!_current) {
        // We've seen no documents yet.
        auto lowerBound = bounds.first;
        _current = lowerBound;
        createDocGenerator(lowerBound,
                           RangeStatement(_range.getStep(),
                                          ExplicitBounds(bounds.first, bounds.second),
                                          _range.getUnit()));
    } else if (_current->increment(_range) >= bounds.second) {
        _densifyState = DensifyState::kDensifyDone;
        return DocumentSource::GetNextResult::makeEOF();
    } else {
        auto lowerBound = _current->increment(_range);
        createDocGenerator(lowerBound,
                           RangeStatement(_range.getStep(),
                                          ExplicitBounds(bounds.first, bounds.second),
                                          _range.getUnit()));
    }
    _densifyState = DensifyState::kHaveGenerator;
    auto generatedDoc = _docGenerator->getNextDocument();
    if (_docGenerator->done()) {
        _densifyState = DensifyState::kDensifyDone;
        _docGenerator = boost::none;
    }
    return generatedDoc;
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::processDocAboveExplicitMinBound(
    Document doc) {
    auto bounds = stdx::get<ExplicitBounds>(_range.getBounds());
    auto val = getDensifyValue(doc);
    // If we are above the range, there must be more left to densify.
    // Otherwise the state would be kDoneDensify and this function would not be reached.
    tassert(8423306,
            "Cannot be in this state if _current is greater than the upper bound.",
            *_current < bounds.second);
    // _current is the last seen value, don't generate it again.
    DensifyValue lowerBound = _current->increment(_range);

    // If val is the next value to be generated, just return it.
    if (val == lowerBound) {
        setPartitionValue(doc);
        _current = lowerBound;
        return doc;
    }

    DensifyValue upperBound = (val < bounds.second) ? val : bounds.second;
    createDocGenerator(
        lowerBound,
        RangeStatement(_range.getStep(), ExplicitBounds(lowerBound, upperBound), _range.getUnit()),
        _partitionExpr ? boost::make_optional<Document>(getDensifyPartition(doc).getDocument())
                       : boost::none,
        doc);
    Document nextFromGen = _docGenerator->getNextDocument();
    _current = getDensifyValue(nextFromGen);
    _densifyState = DensifyState::kHaveGenerator;
    // If the doc generator is done it will be deleted and the state will be kNeedGen.
    resetDocGen(bounds);
    setPartitionValue(nextFromGen);
    return nextFromGen;
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::processFirstDocForExplicitRange(
    Document doc) {
    auto bounds = stdx::get<ExplicitBounds>(_range.getBounds());
    auto val = getDensifyValue(doc);
    // For the first document in a partition, '_current' is the minimum value - step.
    if (!_current) {
        _current = bounds.first.decrement(_range);
    }
    auto where = getPositionRelativeToRange(val);
    switch (where) {
        case ValComparedToRange::kInside: {
            return processDocAboveExplicitMinBound(doc);
        }
        case ValComparedToRange::kAbove: {
            return processDocAboveExplicitMinBound(doc);
        }
        case ValComparedToRange::kRangeMin: {
            _densifyState = DensifyState::kNeedGen;
            _current = val;
            return doc;
        }
        case ValComparedToRange::kBelow: {
            _densifyState = DensifyState::kUninitializedOrBelowRange;
            return doc;
        }
    }
    MONGO_UNREACHABLE_TASSERT(5733414);
    return DocumentSource::GetNextResult::makeEOF();
}

/** Checks if the generator is done, changes states accordingly. */
void DocumentSourceInternalDensify::resetDocGen(ExplicitBounds bounds) {
    if (_docGenerator->done()) {
        if (*_current >= bounds.second && !_partitionExpr) {
            _densifyState = DensifyState::kDensifyDone;
        } else if (_partitionExpr && _eof) {
            _densifyState = DensifyState::kFinishingDensify;
        } else {
            _densifyState = DensifyState::kNeedGen;
        }
        _docGenerator = boost::none;
    }
}

DocumentSourceInternalDensify::ValComparedToRange
DocumentSourceInternalDensify::getPositionRelativeToRange(DensifyValue val) {
    auto bounds = stdx::get<ExplicitBounds>(_range.getBounds());
    int comparison = DensifyValue::compare(val, bounds.first);
    if (comparison < 0) {
        return DocumentSourceInternalDensify::ValComparedToRange::kBelow;
    } else if (comparison == 0) {
        return DocumentSourceInternalDensify::ValComparedToRange::kRangeMin;
    } else if (val < bounds.second) {
        return DocumentSourceInternalDensify::ValComparedToRange::kInside;
    } else {
        return DocumentSourceInternalDensify::ValComparedToRange::kAbove;
    }
}


DocumentSource::GetNextResult DocumentSourceInternalDensify::finishDensifyingPartitionedInputHelper(
    DensifyValue max, boost::optional<DensifyValue> minOverride) {
    while (_partitionTable.size() != 0) {
        auto firstPartitionKeyVal = _partitionTable.begin();
        Value firstPartition = firstPartitionKeyVal->first;
        DensifyValue firstPartitionVal = firstPartitionKeyVal->second;
        // We've already seen the stored value, we want to start generating on the next
        // one.
        auto valToGenerate = firstPartitionVal.increment(_range);
        // If the valToGenerate is > max seen, skip this partition. It is done.
        if (valToGenerate >= max) {
            _partitionTable.erase(firstPartitionKeyVal);
            continue;
        }
        // If the valToGenerate is < 'minOverride', use the override instead.
        if (minOverride && valToGenerate < *minOverride) {
            valToGenerate = *minOverride;
        }
        createDocGenerator(
            valToGenerate,
            RangeStatement(_range.getStep(), ExplicitBounds(valToGenerate, max), _range.getUnit()),
            firstPartition.getDocument(),
            boost::none  // final doc.
        );
        // Remove this partition from the table, we're done with it.
        _partitionTable.erase(firstPartitionKeyVal);
        _densifyState = DensifyState::kHaveGenerator;
        auto nextDoc = _docGenerator->getNextDocument();
        if (_docGenerator->done()) {
            _docGenerator = boost::none;
            _densifyState = DensifyState::kFinishingDensify;
        }
        return nextDoc;
    }
    _densifyState = DensifyState::kDensifyDone;
    return DocumentSource::GetNextResult::makeEOF();
}
DocumentSource::GetNextResult DocumentSourceInternalDensify::finishDensifyingPartitionedInput() {
    // If the partition map is empty, we're done.
    if (_partitionTable.size() == 0) {
        _densifyState = DensifyState::kDensifyDone;
        return DocumentSource::GetNextResult::makeEOF();
    }
    return stdx::visit(
        OverloadedVisitor{
            [&](Full) {
                // Densify between partitions's last seen value and global max.
                tassert(5733707, "_current must be set if partitionTable is non-empty", _current);
                return finishDensifyingPartitionedInputHelper(
                    _globalMax->isOnStepRelativeTo(*_current, _range)
                        ? _globalMax->increment(_range)
                        : *_globalMax);
            },
            [&](Partition) {
                // Partition bounds don't do any extra work after EOF;
                MONGO_UNREACHABLE_TASSERT(5733704);
                return DocumentSource::GetNextResult::makeEOF();
            },
            [&](ExplicitBounds bounds) {
                // Densify between partitions's last seen value and global max. Use the override for
                // the global min.
                return finishDensifyingPartitionedInputHelper(bounds.second, bounds.first);
            }},
        _range.getBounds());
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleSourceExhausted() {
    _eof = true;
    return stdx::visit(
        OverloadedVisitor{
            [&](RangeStatement::Full) {
                if (_partitionExpr) {
                    return finishDensifyingPartitionedInput();
                } else {
                    _densifyState = DensifyState::kDensifyDone;
                    return DocumentSource::GetNextResult::makeEOF();
                }
            },
            [&](RangeStatement::Partition) {
                // We have already densified up to the last document in each partition.
                _densifyState = DensifyState::kDensifyDone;
                return DocumentSource::GetNextResult::makeEOF();
            },
            [&](RangeStatement::ExplicitBounds bounds) {
                if (_partitionExpr) {
                    return finishDensifyingPartitionedInput();
                }
                return densifyExplicitRangeAfterEOF();
            },
        },
        _range.getBounds());
}


DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGen(Document currentDoc) {
    auto val = getDensifyValue(currentDoc);
    auto nextValToGenerate = _current->increment(_range);

    // If the current value is the next value to be generated, save it as the current (last seen)
    // value.
    if (val == nextValToGenerate) {
        setPartitionValue(currentDoc);
        _current = val;
    }
    // If we don't need to create a generator (no intervening documents to generate before
    // outputting currentDoc), then don't create a generator or update _current.
    if (val <= nextValToGenerate) {
        return currentDoc;
    }

    // Falling through the above conditions means the currentDoc is strictly greater than the last
    // seen document plus the step value.
    auto newCurrent = _current->increment(_range);
    createDocGenerator(
        newCurrent,
        RangeStatement(_range.getStep(), ExplicitBounds(newCurrent, val), _range.getUnit()),
        _partitionExpr
            ? boost::make_optional<Document>(getDensifyPartition(currentDoc).getDocument())
            : boost::none,
        currentDoc);

    _densifyState = DensifyState::kHaveGenerator;
    auto nextDoc = _docGenerator->getNextDocument();
    if (_docGenerator->done()) {
        _docGenerator = boost::none;
        _densifyState = DensifyState::kNeedGen;
    }
    // Documents generated by the generator are always on the step.
    _current = getDensifyValue(nextDoc);
    // If we are partitioned, save the most recent doc.
    setPartitionValue(nextDoc);
    return nextDoc;
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGenExplicit(
    Document currentDoc) {
    auto bounds = stdx::get<ExplicitBounds>(_range.getBounds());
    auto val = getDensifyValue(currentDoc);
    auto where = getPositionRelativeToRange(val);
    switch (where) {
        case ValComparedToRange::kInside: {
            auto nextStep = _current->increment(_range);
            if (val == nextStep) {
                _current = val;
                setPartitionValue(currentDoc);
                return currentDoc;
            } else if (val < nextStep) {
                return currentDoc;
            }
            return processDocAboveExplicitMinBound(currentDoc);
        }
        case ValComparedToRange::kAbove: {
            auto nextStep = _current->increment(_range);
            if (nextStep >= bounds.second) {
                _current = nextStep;
                // If we are partitioning other partitions may still need to densify.
                setPartitionValue(currentDoc);
                if (!_partitionExpr) {
                    _densifyState = DensifyState::kDensifyDone;
                }
                return currentDoc;
            }
            return processDocAboveExplicitMinBound(currentDoc);
        }
        case ValComparedToRange::kRangeMin: {
            setPartitionValue(currentDoc);
            _current = val;
            return currentDoc;
        }
        case ValComparedToRange::kBelow: {
            setPartitionValue(currentDoc);
            _densifyState = DensifyState::kUninitializedOrBelowRange;
            return currentDoc;
        }
        default: { MONGO_UNREACHABLE_TASSERT(5733705); }
    }
}
boost::intrusive_ptr<DocumentSource> DocumentSourceInternalDensify::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto results = document_source_densify::createFromBsonInternal(elem, expCtx, kStageName, true);
    tassert(5733413,
            "When creating an $_internalDensify stage, only one stage should be returned",
            results.size() == 1);
    return results.front();
}

Value DocumentSourceInternalDensify::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument spec;
    spec[kFieldFieldName] = Value(_field.fullPath());
    std::vector<Value> serializedPartitionByFields(_partitions.size());
    std::transform(_partitions.begin(),
                   _partitions.end(),
                   serializedPartitionByFields.begin(),
                   [&](FieldPath field) -> Value { return Value(field.fullPath()); });
    spec[kPartitionByFieldsFieldName] = Value(serializedPartitionByFields);
    spec[kRangeFieldName] = _range.serialize();
    MutableDocument out;
    out[getSourceName()] = Value(spec.freeze());

    return Value(out.freezeToValue());
}

void DocumentSourceInternalDensify::initializePartitionState(Document initialDoc) {
    // Initialize _partitionExpr from _partitions.

    // We check whether there is anything in _partitions during parsing.
    tassert(
        6154800, "Expected at least one field when partitioning is enabled.", !_partitions.empty());

    MutableDocument partitionExpr;
    for (auto&& p : _partitions) {
        partitionExpr.setNestedField(p.fullPath(), Value{"$"_sd + p.fullPath()});
    }
    _partitionExpr = ExpressionObject::parse(
        pExpCtx.get(), partitionExpr.freeze().toBson(), pExpCtx->variablesParseState);

    setPartitionValue(initialDoc);
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::doGetNext() {
    // When we return a generated document '_docsGenerated' is incremented. Check that the last
    // document didn't put us over the limit.
    uassert(5897900,
            str::stream() << "Generated " << _docsGenerated
                          << " documents in $densify, which is over the limit of " << _maxDocs
                          << ". Increase the 'internalQueryMaxAllowedDensifyDocs' parameter to "
                             "allow more generated documents",
            _docsGenerated <= _maxDocs);
    switch (_densifyState) {
        case DensifyState::kUninitializedOrBelowRange: {
            // This state represents the first run of doGetNext() or that the value that we last
            // pulled is below the range.

            auto nextDoc = pSource->getNext();
            if (!nextDoc.isAdvanced()) {
                if (nextDoc.isEOF()) {
                    return handleSourceExhausted();
                }
                return nextDoc;
            }

            auto doc = nextDoc.getDocument();
            if (doc.getNestedField(_field).nullish()) {
                // The densify field is not present or null, let document pass unmodified.
                return nextDoc;
            }

            auto val = getDensifyValue(doc);

            // If we have partitions specified, setup the partition expression and table.
            if (_partitions.size() != 0 && !_partitionExpr) {
                initializePartitionState(doc);
            }

            return stdx::visit(
                OverloadedVisitor{
                    [&](Full) {
                        _current = val;
                        _globalMin = val;
                        _globalMax = val;
                        _densifyState = DensifyState::kNeedGen;
                        return nextDoc;
                    },
                    [&](Partition) {
                        tassert(5734400,
                                "Partition state must be initialized for partition bounds",
                                _partitionExpr);
                        _densifyState = DensifyState::kNeedGen;
                        return nextDoc;
                    },
                    [&](ExplicitBounds bounds) { return processFirstDocForExplicitRange(doc); }},
                _range.getBounds());
        }
        case DensifyState::kNeedGen: {
            tassert(8423305, "Document generator must not exist in this state.", !_docGenerator);

            auto nextDoc = pSource->getNext();
            if (!nextDoc.isAdvanced()) {
                if (nextDoc.isEOF()) {
                    return handleSourceExhausted();
                }
                return nextDoc;
            }

            auto currentDoc = nextDoc.getDocument();
            if (currentDoc.getNestedField(_field).nullish()) {
                // The densify field is not present or null, let document pass unmodified.
                return nextDoc;
            }
            auto val = getDensifyValue(currentDoc);

            return stdx::visit(
                OverloadedVisitor{[&](Full) {
                                      if (_partitionExpr) {
                                          // Keep track of '_globalMax' for later. The latest
                                          // document from the source is always the max.
                                          _globalMax = val;
                                          // If we haven't seen this partition before, densify
                                          // between
                                          // '_globalMin' and this value.
                                          auto partitionVal = getDensifyPartition(currentDoc);
                                          auto foundPartitionVal =
                                              _partitionTable.find(partitionVal);
                                          if (foundPartitionVal == _partitionTable.end()) {
                                              // _current represents the last value seen. We want to
                                              // generate _globalMin, so pretend we've seen the
                                              // value before that.
                                              _current = _globalMin->decrement(_range);
                                              // Insert the new partition into the table.
                                              setPartitionValue(currentDoc);
                                              return handleNeedGen(currentDoc);
                                          }
                                          // Otherwise densify between the last seen value and this
                                          // one.
                                          _current = foundPartitionVal->second;
                                      }
                                      return handleNeedGen(currentDoc);
                                  },
                                  [&](Partition) {
                                      // If we haven't seen this partition before, add it to the
                                      // table then return.
                                      auto partitionVal = getDensifyPartition(currentDoc);
                                      auto foundPartitionVal = _partitionTable.find(partitionVal);
                                      if (foundPartitionVal == _partitionTable.end()) {
                                          setPartitionValue(currentDoc);
                                          return nextDoc;
                                      }
                                      // Reset current to be the last value in this partition.
                                      _current = foundPartitionVal->second;
                                      return handleNeedGen(currentDoc);
                                  },
                                  [&](ExplicitBounds bounds) {
                                      if (_partitionExpr) {
                                          // If we haven't seen this partition before, add it to the
                                          // table then check where it is in the range.
                                          auto partitionVal = getDensifyPartition(currentDoc);
                                          auto foundPartitionVal =
                                              _partitionTable.find(partitionVal);
                                          if (foundPartitionVal == _partitionTable.end()) {
                                              setPartitionValue(currentDoc);
                                              // This partition has seen no values.
                                              _current = boost::none;
                                              return processFirstDocForExplicitRange(currentDoc);
                                          }
                                          // Otherwise reset current to be the last value in this
                                          // partition.
                                          _current = foundPartitionVal->second;
                                      }
                                      return handleNeedGenExplicit(nextDoc.getDocument());
                                  }},
                _range.getBounds());
        }
        case DensifyState::kHaveGenerator: {
            tassert(5733203,
                    "Densify state is kHaveGenerator but DocGenerator is null or done.",
                    _docGenerator && !_docGenerator->done());

            auto generatedDoc = _docGenerator->getNextDocument();

            return stdx::visit(
                OverloadedVisitor{[&](Full) {
                                      if (_docGenerator->done()) {
                                          _docGenerator = boost::none;
                                          if (_eof && _partitionExpr) {
                                              _densifyState = DensifyState::kFinishingDensify;
                                          } else {
                                              _densifyState = DensifyState::kNeedGen;
                                          }
                                      }
                                      // The generator's final document may not be on the
                                      // step.
                                      auto genDensifyVal = getDensifyValue(generatedDoc);
                                      if (genDensifyVal == _current->increment(_range)) {
                                          _current = genDensifyVal;
                                          setPartitionValue(generatedDoc);
                                      }
                                      return generatedDoc;
                                  },
                                  [&](Partition) {
                                      if (_docGenerator->done()) {
                                          _docGenerator = boost::none;
                                          _densifyState = DensifyState::kNeedGen;
                                      }
                                      // The generator's final document may not be on the
                                      // step.
                                      auto genDensifyVal = getDensifyValue(generatedDoc);
                                      if (genDensifyVal == _current->increment(_range)) {
                                          _current = genDensifyVal;
                                          setPartitionValue(generatedDoc);
                                      }
                                      return generatedDoc;
                                  },
                                  [&](ExplicitBounds bounds) {
                                      auto val = getDensifyValue(generatedDoc);
                                      // Only want to update the rangeMin if the value -
                                      // current is divisible by the step.
                                      if (val.isOnStepRelativeTo(*_current, _range)) {
                                          _current = val;
                                          setPartitionValue(generatedDoc);
                                      }
                                      resetDocGen(bounds);
                                      return generatedDoc;
                                  }},
                _range.getBounds());
        }
        case DensifyState::kFinishingDensify: {
            tassert(5734402,
                    "Densify expected to have already hit EOF in FinishingDensify state",
                    _eof);
            return finishDensifyingPartitionedInput();
        }
        case DensifyState::kDensifyDone: {
            // In the full range, this should only return EOF.
            // In the explicit range we finish densifying over the range and any remaining documents
            // is passed to the next stage.
            auto doc = pSource->getNext();
            if (stdx::holds_alternative<Full>(_range.getBounds())) {
                tassert(5734005,
                        "GetNextResult must be EOF in kDensifyDone and kFull state",
                        !doc.isAdvanced());
            }
            return doc;
        }
        default: { MONGO_UNREACHABLE_TASSERT(5733706); }
    }  // namespace mongo
}

DensifyValue DensifyValue::increment(const RangeStatement& range) const {
    return stdx::visit(
        OverloadedVisitor{
            [&](Value val) {
                return DensifyValue(uassertStatusOK(ExpressionAdd::apply(val, range.getStep())));
            },
            [&](Date_t date) {
                return DensifyValue(dateAdd(
                    date, range.getUnit().value(), range.getStep().coerceToLong(), timezone()));
            }},
        _value);
}

DensifyValue DensifyValue::decrement(const RangeStatement& range) const {
    return stdx::visit(
        OverloadedVisitor{
            [&](Value val) {
                return DensifyValue(
                    uassertStatusOK(ExpressionSubtract::apply(val, range.getStep())));
            },
            [&](Date_t date) {
                return DensifyValue(dateAdd(
                    date, range.getUnit().value(), -range.getStep().coerceToLong(), timezone()));
            }},
        _value);
}

bool DensifyValue::isOnStepRelativeTo(DensifyValue base, RangeStatement range) const {
    return stdx::visit(
        OverloadedVisitor{
            [&](Value val) {
                Value diff = uassertStatusOK(ExpressionSubtract::apply(val, base.getNumber()));
                Value remainder = uassertStatusOK(ExpressionMod::apply(diff, range.getStep()));
                return remainder.getDouble() == 0.0;
            },
            [&](Date_t date) {
                auto unit = range.getUnit().value();
                long long step = range.getStep().coerceToLong();
                auto baseDate = base.getDate();

                // Months, quarters and years have variable lengths depending on leap days
                // and days-in-a-month, so a step is not a constant number of milliseconds
                // across all dates. For these units, we need to iterate through rather than
                // performing a calculation with modulo. As long as `baseDate` is not a large number
                // of steps away from the value we're testing (as is true in our usage with _current
                // as the base), this should not be a performance issue.
                if (unit == TimeUnit::month || unit == TimeUnit::quarter ||
                    unit == TimeUnit::year) {

                    Date_t steppedDate = baseDate;
                    while (steppedDate < date) {
                        steppedDate = dateAdd(steppedDate, unit, step, timezone());
                    }
                    return steppedDate == date;
                } else {
                    // Steps with units smaller than one month are always constant sized
                    // (because unix time does not have leap seconds), so we can perform
                    // modulo arithmetic.
                    auto testMillis = date.toMillisSinceEpoch();
                    auto baseMillis = baseDate.toMillisSinceEpoch();
                    auto stepDurationInMillis =
                        dateAdd(Date_t::fromMillisSinceEpoch(0), unit, step, timezone())
                            .toMillisSinceEpoch();
                    auto diff = testMillis - baseMillis;
                    return diff % stepDurationInMillis == 0;
                }
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
    if (_partitions.size() != 0 && stdx::holds_alternative<Full>(_range.getBounds())) {
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
