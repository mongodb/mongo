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
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/visit_helper.h"

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using SortPatternPart = mongo::SortPattern::SortPatternPart;
using NumericBounds = mongo::RangeStatement::NumericBounds;
using DateBounds = mongo::RangeStatement::DateBounds;
using Full = mongo::RangeStatement::Full;
using Partition = mongo::RangeStatement::Partition;

namespace mongo {

RangeStatement RangeStatement::parse(RangeSpec spec) {
    Value step = spec.getStep();
    ValueComparator comp = ValueComparator();
    uassert(5733401,
            "The step parameter in a range statement must be a strictly positive numeric value",
            step.numeric() && comp.evaluate(step > Value(0)));

    optional<TimeUnit> unit = [&]() {
        if (auto unit = spec.getUnit()) {
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
                    return Bounds(std::pair<Value, Value>(Value(array[0]), Value(array[1])));
                } else if (array[0].type() == mongo::Date) {
                    uassert(5733405,
                            "A bounding array must contain either both dates or both numeric types",
                            array[1].type() == mongo::Date);
                    uassert(5733410, "A bounding array of dates must specify a unit", unit);
                    return Bounds(std::pair<Date_t, Date_t>(array[0].date(), array[1].date()));
                }
            }
            case mongo::String: {
                if (bounds.str() == kValFull)
                    return Bounds(Full());
                else if (bounds.str() == kValPartition)
                    return Bounds(Partition());
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

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    densify,
    LiteParsedDocumentSourceDefault::parse,
    document_source_densify::createFromBson,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    multiversion::FeatureCompatibilityVersion::kVersion_5_1,
    ::mongo::feature_flags::gFeatureFlagDensify.isEnabledAndIgnoreFCV());

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    _internalDensify,
    LiteParsedDocumentSourceDefault::parse,
    DocumentSourceInternalDensify::createFromBson,
    AllowedWithApiStrict::kInternal,
    AllowedWithClientType::kInternal,
    multiversion::FeatureCompatibilityVersion::kVersion_5_1,
    ::mongo::feature_flags::gFeatureFlagDensify.isEnabledAndIgnoreFCV());

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

    auto spec = DensifySpec::parse(IDLParserErrorContext(stageName), elem.embeddedObject());
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

list<intrusive_ptr<DocumentSource>> create(const intrusive_ptr<ExpressionContext>& expCtx,
                                           list<FieldPath> partitions,
                                           FieldPath field,
                                           RangeStatement rangeStatement,
                                           bool isInternal) {
    list<intrusive_ptr<DocumentSource>> results;

    // If we're creating an internal stage then we must not desugar and produce a sort stage in
    // addition.
    if (!isInternal) {
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

        // Constructing resulting stages.
        results.push_back(DocumentSourceSort::create(expCtx, SortPattern{sortParts}));
    }

    // Constructing resulting stages.
    results.push_back(
        make_intrusive<DocumentSourceInternalDensify>(expCtx, field, partitions, rangeStatement));

    return results;
}
}  // namespace document_source_densify

namespace {
Value getDensifyValue(const Document& doc, const FieldPath& path) {
    Value val = doc.getNestedField(path);
    uassert(5733201, "Densify field type must be numeric", val.numeric());
    return val;
}

Value addValues(Value lhs, Value rhs) {
    return uassertStatusOK(ExpressionAdd::apply(lhs, rhs));
}

Value subtractValues(Value lhs, Value rhs) {
    return uassertStatusOK(ExpressionSubtract::apply(lhs, rhs));
}

Value floorValue(Value operand) {
    return uassertStatusOK(ExpressionFloor::apply(operand));
}
}  // namespace

DocumentSourceInternalDensify::DocGenerator::DocGenerator(
    DocumentSourceInternalDensify::DensifyValueType min,
    RangeStatement range,
    FieldPath fieldName,
    boost::optional<Document> includeFields,
    boost::optional<Document> finalDoc,
    ValueComparator comp)
    : _comp(std::move(comp)),
      _range(std::move(range)),
      _path(std::move(fieldName.fullPath())),
      _finalDoc(std::move(finalDoc)),
      _min(std::move(min)) {

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
    stdx::visit(
        visit_helper::Overloaded{
            [&](const Value val) {
                tassert(5733304,
                        "DocGenerator all values must be same type",
                        stdx::holds_alternative<NumericBounds>(_range.getBounds()));
                NumericBounds bounds = stdx::get<NumericBounds>(_range.getBounds());
                tassert(5733303,
                        "DocGenerator min must be lower or equal to max",
                        _comp.evaluate(bounds.second >= val));
                tassert(
                    5733506, "Unit must not be specified with non-date values", !_range.getUnit());
            },
            [&](const Date_t dateMin) {
                tassert(5733500,
                        "DocGenerator all values must be same type",
                        stdx::holds_alternative<DateBounds>(_range.getBounds()));
                DateBounds bounds = stdx::get<DateBounds>(_range.getBounds());
                tassert(5733501, "Unit must be specified with a date step", _range.getUnit());
                Value floorStep = floorValue(_range.getStep());
                tassert(5733505,
                        "Step must be an integer for date densification",
                        _comp.evaluate(floorStep == _range.getStep()));
                tassert(5733502,
                        "DocGenerator min must be lower or equal to max",
                        bounds.second >= dateMin);
            },
        },
        _min);
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
    Value valueToAdd;
    stdx::visit(
        visit_helper::Overloaded{
            [&](Value val) {
                valueToAdd = val;
                Value nextValue = addValues(val, _range.getStep());
                NumericBounds bounds = stdx::get<NumericBounds>(_range.getBounds());
                if (_comp.evaluate(nextValue > bounds.second)) {
                    _state =
                        _finalDoc ? GeneratorState::kReturningFinalDocument : GeneratorState::kDone;
                }
                _min = nextValue;
            },
            [&](Date_t dateVal) {
                valueToAdd = Value(dateVal);
                dateVal = dateAdd(dateVal,
                                  _range.getUnit().get(),
                                  _range.getStep().getDouble(),
                                  TimeZoneDatabase::utcZone());
                DateBounds bounds = stdx::get<DateBounds>(_range.getBounds());
                if (dateVal > bounds.second) {
                    _state =
                        _finalDoc ? GeneratorState::kReturningFinalDocument : GeneratorState::kDone;
                }
                _min = dateVal;
            },
        },
        _min);

    MutableDocument retDoc(_includeFields);
    retDoc.setNestedField(_path, valueToAdd);
    return retDoc.freeze();
}

bool DocumentSourceInternalDensify::DocGenerator::done() const {
    return _state == GeneratorState::kDone;
}

// TODO SERVER-57344: Execution flow should be refactored such that std::visits are done in these
// functions instead of the doGetNext(). This is to avoid the need to pass NumericBounds to avoid
// duplicate std::visits in functions like this.
DocumentSource::GetNextResult DocumentSourceInternalDensify::densifyAfterEOF(NumericBounds bounds) {
    // Once we have hit an EOF, if the last seen value (_current) plus the step is greater
    // than or equal to the rangeMax, that means we have finished densifying
    // over the explicit range so we just return an EOF. Otherwise, we finish
    // densifying over the rest of the range.
    if (compareValues(addValues(stdx::get<Value>(*_current), _range.getStep()) >= bounds.second)) {
        _densifyState = DensifyState::kDensifyDone;
        return DocumentSource::GetNextResult::makeEOF();
    } else {
        _docGenerator = DocGenerator(addValues(stdx::get<Value>(*_current), _range.getStep()),
                                     _range,
                                     _field,
                                     boost::none,
                                     boost::none,
                                     pExpCtx->getValueComparator());
        _densifyState = DensifyState::kHaveGenerator;
        return _docGenerator->getNextDocument();
    }
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::processDocAboveMinBound(
    Value val, NumericBounds bounds, Document doc) {
    // If we are above the range, there must be more left to densify.
    // Otherwise the state would be kDoneDensify and this function would not be reached.
    tassert(8423306,
            "Cannot be in this state if _current is greater than the upper bound.",
            compareValues(stdx::get<Value>(*_current) <= bounds.second));
    auto rem = valOffsetFromStep(val, stdx::get<Value>(*_current), _range.getStep());
    // If val is on the step we need to subtract the step to avoid returning the doc twice.
    if (compareValues(rem == Value(0))) {
        val = subtractValues(val, _range.getStep());
    }
    Value upperBound = (compareValues(val <= bounds.second)) ? val : bounds.second;

    _docGenerator =
        DocGenerator(*_current,
                     RangeStatement(_range.getStep(),
                                    NumericBounds(stdx::get<Value>(*_current), upperBound),
                                    _range.getUnit()),
                     _field,
                     boost::none,
                     std::move(doc),
                     pExpCtx->getValueComparator());
    Document nextFromGen = _docGenerator->getNextDocument();
    _current = getDensifyValue(nextFromGen, _field);
    _densifyState = DensifyState::kHaveGenerator;
    // If the doc generator is done it will be deleted and the state will be kNeedGen.
    resetDocGen(bounds);
    return nextFromGen;
}

/** Checks if the generator is done, changes states accordingly. */
void DocumentSourceInternalDensify::resetDocGen(NumericBounds bounds) {
    if (_docGenerator->done()) {
        if (compareValues(stdx::get<Value>(*_current) >= bounds.second)) {
            _densifyState = DensifyState::kDensifyDone;
        } else {
            _densifyState = DensifyState::kNeedGen;
        }
        _docGenerator = boost::none;
    }
}


DocumentSourceInternalDensify::ValComparedToRange DocumentSourceInternalDensify::processRange(
    Value val, Value current, NumericBounds bounds) {
    int comparison = compareValues(val, current);
    if (comparison < 0) {
        return DocumentSourceInternalDensify::ValComparedToRange::kBelow;
    } else if (comparison == 0) {
        return DocumentSourceInternalDensify::ValComparedToRange::kRangeMin;
    } else if (compareValues(val <= bounds.second)) {
        return DocumentSourceInternalDensify::ValComparedToRange::kInside;
    } else {
        return DocumentSourceInternalDensify::ValComparedToRange::kAbove;
    }
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleSourceExhausted() {
    _eof = true;
    return stdx::visit(
        visit_helper::Overloaded{
            [&](RangeStatement::Full full) {
                _densifyState = DensifyState::kDensifyDone;
                return DocumentSource::GetNextResult::makeEOF();
            },
            [&](RangeStatement::Partition part) {
                MONGO_UNREACHABLE;
                return DocumentSource::GetNextResult::makeEOF();
            },
            [&](RangeStatement::DateBounds bounds) {
                // TODO SERVER-57340 and SERVER-57342
                tasserted(5734000, "Type of densify should not be kPartition");
                return DocumentSource::GetNextResult::makeEOF();
            },
            [&](RangeStatement::NumericBounds bounds) {
                // The _current is treated as the last seen value. Therefore, when creating document
                // generators we pass in the _current + the step in order to avoid returning the
                // same document twice. However, if we have yet to densify, we do not want to skip
                // the current value of _current, so the step is decremented here to avoid that.
                if (_densifyState == DensifyState::kUninitializedOrBelowRange) {
                    _current = subtractValues(stdx::get<Value>(*_current), _range.getStep());
                }
                return densifyAfterEOF(bounds);
            },
        },
        _range.getBounds());
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGenFull(Document currentDoc,
                                                                               Value max) {
    // Note that max is not the global max, its only the max up to the current document.
    Value currentPlusStep = addValues(stdx::get<Value>(*_current), _range.getStep());

    if (compareValues(currentPlusStep >= max)) {
        _current = max;
        return currentDoc;
    }

    // This line checks if we are aligned on the step by checking if the current
    // value in the document minus the min is divisible by the step. If it is we
    // subtract step from max. This is neccessary so we don't generate the final
    // document twice.
    auto offsetFromStep = valOffsetFromStep(max, stdx::get<Value>(*_current), _range.getStep());
    auto maxAdjusted =
        compareValues(offsetFromStep == Value(0)) ? subtractValues(max, _range.getStep()) : max;

    Value newCurrent = addValues(stdx::get<Value>(*_current), _range.getStep());
    _docGenerator = DocumentSourceInternalDensify::DocGenerator(
        DensifyValueType(newCurrent),
        RangeStatement(_range.getStep(), NumericBounds(newCurrent, maxAdjusted), _range.getUnit()),
        _field,
        boost::none,
        std::move(currentDoc),
        pExpCtx->getValueComparator());

    _densifyState = DensifyState::kHaveGenerator;
    auto nextDoc = _docGenerator->getNextDocument();
    if (_docGenerator->done()) {
        _docGenerator = boost::none;
        _densifyState = DensifyState::kNeedGen;
    }
    _current = getDensifyValue(nextDoc, _field);

    return nextDoc;
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGenExplicit(
    Document currentDoc, Value val, NumericBounds bounds) {
    auto where = processRange(val, stdx::get<Value>(*_current), bounds);
    switch (where) {
        case ValComparedToRange::kInside: {
            _current = addValues(stdx::get<Value>(*_current), _range.getStep());
            if (compareValues(stdx::get<Value>(*_current) == val)) {
                return currentDoc;
            }
            return processDocAboveMinBound(val, bounds, currentDoc);
        }
        case ValComparedToRange::kAbove: {
            _current = addValues(stdx::get<Value>(*_current), _range.getStep());
            if (compareValues(stdx::get<Value>(*_current) > bounds.second)) {
                _densifyState = DensifyState::kDensifyDone;
                return currentDoc;
            }
            return processDocAboveMinBound(val, bounds, currentDoc);
        }
        case ValComparedToRange::kRangeMin: {
            _current = addValues(stdx::get<Value>(*_current), _range.getStep());
            _densifyState = DensifyState::kUninitializedOrBelowRange;
            return currentDoc;
        }
        case ValComparedToRange::kBelow: {
            _densifyState = DensifyState::kUninitializedOrBelowRange;
            return currentDoc;
        }
        default: { MONGO_UNREACHABLE; }
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

DocumentSource::GetNextResult DocumentSourceInternalDensify::doGetNext() {
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
            if (doc.getNestedField(_field).missing()) {
                // The densify field is not present, let document pass unmodified.
                return nextDoc;
            }

            Value val = getDensifyValue(doc, _field);

            return stdx::visit(
                visit_helper::Overloaded{
                    [&](Full full) {
                        _current = val;
                        _densifyState = DensifyState::kNeedGen;
                        return nextDoc;
                    },
                    [&](Partition partition) {
                        tasserted(5734001, "Type of densify should not be 'partition'");
                        return DocumentSource::GetNextResult::makeEOF();
                    },
                    [&](DateBounds bounds) {
                        tasserted(5733412, "Type of densify should not be date bounds");
                        return DocumentSource::GetNextResult::makeEOF();
                    },
                    [&](NumericBounds bounds) {
                        auto where = processRange(val, stdx::get<Value>(*_current), bounds);
                        switch (where) {
                            case ValComparedToRange::kInside: {
                                return processDocAboveMinBound(val, bounds, nextDoc.getDocument());
                            }
                            case ValComparedToRange::kAbove: {
                                return processDocAboveMinBound(val, bounds, nextDoc.getDocument());
                            }
                            case ValComparedToRange::kRangeMin: {
                                _current = addValues(stdx::get<Value>(*_current), _range.getStep());
                                return nextDoc;
                            }
                            case ValComparedToRange::kBelow: {
                                return nextDoc;
                            }
                        }
                        tasserted(5733414, "One of the switch statements should have been hit.");
                        return DocumentSource::GetNextResult::makeEOF();
                    }},
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
            if (currentDoc.getNestedField(_field).missing()) {
                // The densify field is not present, let document pass unmodified.
                return nextDoc;
            }
            Value val = getDensifyValue(currentDoc, _field);

            return stdx::visit(
                visit_helper::Overloaded{
                    [&](Full full) {
                        _current = *_current;
                        return handleNeedGenFull(currentDoc, val);
                    },
                    [&](Partition partition) {
                        // TODO SERVER-57340 and SERVER-57342
                        tasserted(5734003, "Type of densify should not be kPartition");
                        return DocumentSource::GetNextResult::makeEOF();
                    },
                    [&](DateBounds bounds) {
                        MONGO_UNREACHABLE;
                        return DocumentSource::GetNextResult::makeEOF();
                    },
                    [&](NumericBounds bounds) {
                        return handleNeedGenExplicit(nextDoc.getDocument(), val, bounds);
                    }},
                _range.getBounds());
        }
        case DensifyState::kHaveGenerator: {
            tassert(5733203,
                    "Densify state is kHaveGenerator but DocGenerator is null or done.",
                    _docGenerator && !_docGenerator->done());

            auto generatedDoc = _docGenerator->getNextDocument();

            return stdx::visit(
                visit_helper::Overloaded{
                    [&](Full full) {
                        if (_docGenerator->done()) {
                            _docGenerator = boost::none;
                            _densifyState = DensifyState::kNeedGen;
                        }
                        _current = getDensifyValue(generatedDoc, _field);
                        return GetNextResult(std::move(generatedDoc));
                    },
                    [&](Partition partition) {
                        // TODO SERVER-57340 and SERVER-57342
                        tasserted(5734004, "Type of densify should not be kPartition");
                        return DocumentSource::GetNextResult::makeEOF();
                    },
                    [&](DateBounds bounds) {
                        MONGO_UNREACHABLE;
                        return DocumentSource::GetNextResult::makeEOF();
                    },
                    [&](NumericBounds bounds) {
                        auto val = getDensifyValue(generatedDoc, _field);
                        // Only want to update the rangeMin if the value - current is divisible by
                        // the step.
                        auto rem =
                            valOffsetFromStep(val, stdx::get<Value>(*_current), _range.getStep());
                        if (compareValues(rem == Value(0))) {
                            _current = val;
                        }
                        resetDocGen(bounds);
                        return GetNextResult(std::move(generatedDoc));
                    }},
                _range.getBounds());
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
        default: { MONGO_UNREACHABLE; }
    }
}
}  // namespace mongo
