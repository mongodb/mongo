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
#include "mongo/db/pipeline/field_path.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/visit_helper.h"

namespace mongo {
namespace document_source_densify {
// TODO SERVER-57334 Translation logic goes here.
}  // namespace document_source_densify

namespace {
double getDensifyDouble(const Document& doc, const FieldPath& path) {
    Value val = doc.getNestedField(path);
    uassert(5733201, "Densify field type must be double", val.numeric());
    return val.getDouble();
}
}  // namespace

DocumentSourceInternalDensify::DocGenerator::DocGenerator(
    DocumentSourceInternalDensify::DensifyValueType min,
    DocumentSourceInternalDensify::DensifyValueType max,
    StepSpec step,
    FieldPath fieldName,
    boost::optional<Document> includeFields,
    boost::optional<Document> finalDoc)
    : _step(std::move(step)),
      _path(std::move(fieldName.fullPath())),
      _finalDoc(std::move(finalDoc)),
      _min(std::move(min)),
      _max(std::move(max)) {

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

    tassert(5733305, "DocGenerator step must be positive", _step.step > 0);
    stdx::visit(
        visit_helper::Overloaded{
            [&](const double doubleMin) {
                tassert(5733304,
                        "DocGenerator all values must be same type",
                        stdx::holds_alternative<double>(_max));
                tassert(5733303,
                        "DocGenerator min must be lower or equal to max",
                        stdx::get<double>(_max) >= doubleMin);
                tassert(5733506,
                        "Unit and tz must not be specified with non-date values",
                        !_step.unit && !_step.tz);
            },
            [&](const Date_t dateMin) {
                tassert(5733500,
                        "DocGenerator all values must be same type",
                        stdx::holds_alternative<Date_t>(_max));
                tassert(5733501, "Unit must be specified with a date step", _step.unit);
                tassert(5733505,
                        "Step must be an integer for date densification",
                        floor(_step.step) == _step.step);
                tassert(5733502,
                        "DocGenerator min must be lower or equal to max",
                        stdx::get<Date_t>(_max) >= dateMin);
                tassert(5733504, "DocGenerator with dates requires a time zone", _step.tz);
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
            [&](double doubleVal) {
                valueToAdd = Value(doubleVal);
                doubleVal += _step.step;
                if (doubleVal > stdx::get<double>(_max)) {
                    _state =
                        _finalDoc ? GeneratorState::kReturningFinalDocument : GeneratorState::kDone;
                }
                _min = doubleVal;
            },
            [&](Date_t dateVal) {
                valueToAdd = Value(dateVal);
                dateVal = dateAdd(dateVal, _step.unit.get(), _step.step, _step.tz.get());
                if (dateVal > stdx::get<Date_t>(_max)) {
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

DocumentSourceInternalDensify::DocumentSourceInternalDensify(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    double step,
    FieldPath path,
    boost::optional<std::pair<DensifyValueType, DensifyValueType>> range)
    : DocumentSource(kStageName, pExpCtx), _step{step}, _path(std::move(path)) {
    if (range) {
        _rangeMin = range->first;
        _rangeMax = range->second;
        _densifyType = TypeOfDensify::kExplicitRange;
    } else {
        _densifyType = TypeOfDensify::kFull;
    }
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::densifyAfterEOF() {
    // Once we have hit an EOF, if the last seen value (_rangeMin) plus the step is greater
    // than or equal to the rangeMax, that means we have finished densifying
    // over the explicit range so we just return an EOF. Otherwise, we finish
    // densifying over the rest of the range.
    if (stdx::get<double>(*_rangeMin) + _step.step >= stdx::get<double>(*_rangeMax)) {
        _densifyState = DensifyState::kDensifyDone;
        return DocumentSource::GetNextResult::makeEOF();
    } else {
        _docGenerator = DocGenerator(stdx::get<double>(*_rangeMin) + _step.step,
                                     *_rangeMax,
                                     _step,
                                     _path,
                                     boost::none,
                                     boost::none);
        _densifyState = DensifyState::kHaveGenerator;
        return _docGenerator->getNextDocument();
    }
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::processDocAboveMinBound(double val,
                                                                                     Document doc) {
    // If we are above the range, there must be more left to densify.
    // Otherwise the state would be kDoneDensify and this function would not be reached.
    tassert(8423306,
            "Cannot be in this state if _rangeMin is greater than _rangeMax.",
            *_rangeMin <= *_rangeMax);
    auto rem = valOffsetFromStep(val, stdx::get<double>(*_rangeMin), _step.step);
    // If val is on the step we need to subtract the step to avoid returning the doc twice.
    if (rem == 0) {
        val = val - _step.step;
    }
    _docGenerator = DocGenerator(*_rangeMin,
                                 std::min(val, stdx::get<double>(*_rangeMax)),
                                 _step,
                                 _path,
                                 boost::none,
                                 std::move(doc));
    Document nextFromGen = _docGenerator->getNextDocument();
    _rangeMin = getDensifyDouble(nextFromGen, _path);
    _densifyState = DensifyState::kHaveGenerator;
    // If the doc generator is done it will be deleted and the state will be kNeedGen.
    resetDocGen();
    return nextFromGen;
}

/** Checks if the generator is done, changes states accordingly. */
void DocumentSourceInternalDensify::resetDocGen() {
    if (_docGenerator->done()) {
        if (_rangeMin >= _rangeMax) {
            _densifyState = DensifyState::kDensifyDone;
        } else {
            _densifyState = DensifyState::kNeedGen;
        }
        _docGenerator = boost::none;
    }
}


DocumentSourceInternalDensify::ValComparedToRange DocumentSourceInternalDensify::processRangeNum(
    double val, double rangeMin, double rangeMax) {
    if (val < rangeMin) {
        return DocumentSourceInternalDensify::ValComparedToRange::kBelow;
    } else if (val == rangeMin) {
        return DocumentSourceInternalDensify::ValComparedToRange::kRangeMin;
    } else if (val > rangeMin && val <= rangeMax) {
        return DocumentSourceInternalDensify::ValComparedToRange::kInside;
    } else {
        return DocumentSourceInternalDensify::ValComparedToRange::kAbove;
    }
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleSourceExhausted() {
    _eof = true;
    switch (_densifyType) {
        case TypeOfDensify::kFull: {
            _densifyState = DensifyState::kDensifyDone;
            return DocumentSource::GetNextResult::makeEOF();
        }
        case TypeOfDensify::kExplicitRange: {
            // The _rangeMin is treated as the last seen value. Therefore, when creating document
            // generators we pass in the _rangeMin + the step in order to avoid returning the same
            // document twice. However, if we have yet to densify, we do not want to skip the
            // current value of _rangeMin, so the step is decremented here to avoid that.
            if (_densifyState == DensifyState::kUninitializedOrBelowRange) {
                _rangeMin = stdx::get<double>(*_rangeMin) - _step.step;
            }
            return densifyAfterEOF();
        }
        case TypeOfDensify::kPartition: {
            // TODO SERVER-57340 and SERVER-57342
            tasserted(5734000, "TypefDensify should not be kPartition");
            break;
        }
        default: { MONGO_UNREACHABLE; }
    }
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGenFull(
    Document currentDoc) {
    // Note that _rangeMax is not the global max, its only the max up to the current document.
    if (stdx::get<double>(*_rangeMin) + _step.step >= stdx::get<double>(*_rangeMax)) {
        _rangeMin = stdx::get<double>(*_rangeMax);
        return currentDoc;
    }

    // This line checks if we are aligned on the step by checking if the current
    // value in the document minus the min is divisible by the step. If it is we
    // subtract step from max. This is neccessary so we don't generate the final
    // document twice.
    auto maxAdjusted = valOffsetFromStep(stdx::get<double>(*_rangeMax),
                                         stdx::get<double>(*_rangeMin),
                                         _step.step) == 0
        ? stdx::get<double>(*_rangeMax) - _step.step
        : stdx::get<double>(*_rangeMax);

    _docGenerator =
        DocumentSourceInternalDensify::DocGenerator(stdx::get<double>(*_rangeMin) + _step.step,
                                                    maxAdjusted,
                                                    _step,
                                                    _path,
                                                    boost::none,
                                                    std::move(currentDoc));

    _densifyState = DensifyState::kHaveGenerator;
    auto nextDoc = _docGenerator->getNextDocument();
    if (_docGenerator->done()) {
        _docGenerator = boost::none;
        _densifyState = DensifyState::kNeedGen;
    }
    _rangeMin = getDensifyDouble(nextDoc, _path);

    return nextDoc;
}

DocumentSource::GetNextResult DocumentSourceInternalDensify::handleNeedGenExplicit(
    Document currentDoc, double val) {
    auto where = processRangeNum(val, stdx::get<double>(*_rangeMin), stdx::get<double>(*_rangeMax));
    switch (where) {
        case ValComparedToRange::kInside: {
            _rangeMin = stdx::get<double>(*_rangeMin) + _step.step;
            if (stdx::get<double>(*_rangeMin) == val) {
                return currentDoc;
            }
            return processDocAboveMinBound(val, currentDoc);
        }
        case ValComparedToRange::kAbove: {
            _rangeMin = stdx::get<double>(*_rangeMin) + _step.step;
            if (stdx::get<double>(*_rangeMin) > stdx::get<double>(*_rangeMax)) {
                _densifyState = DensifyState::kDensifyDone;
                return currentDoc;
            }
            return processDocAboveMinBound(val, currentDoc);
        }
        case ValComparedToRange::kRangeMin: {
            _rangeMin = stdx::get<double>(*_rangeMin) + _step.step;
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
            if (doc.getNestedField(_path).missing()) {
                // The densify field is not present, let document pass unmodified.
                return nextDoc;
            }
            double val = getDensifyDouble(doc, _path);

            switch (_densifyType) {
                case TypeOfDensify::kFull: {
                    _rangeMin = val;
                    _densifyState = DensifyState::kNeedGen;
                    return nextDoc;
                }
                case TypeOfDensify::kExplicitRange: {
                    auto where = processRangeNum(
                        val, stdx::get<double>(*_rangeMin), stdx::get<double>(*_rangeMax));
                    switch (where) {
                        case ValComparedToRange::kInside: {
                            return processDocAboveMinBound(val, nextDoc.getDocument());
                        }
                        case ValComparedToRange::kAbove: {
                            return processDocAboveMinBound(val, nextDoc.getDocument());
                        }
                        case ValComparedToRange::kRangeMin: {
                            _rangeMin = stdx::get<double>(*_rangeMin) + _step.step;
                            return nextDoc;
                        }
                        case ValComparedToRange::kBelow: {
                            return nextDoc;
                        }
                    }
                }
                case TypeOfDensify::kPartition: {
                    // TODO SERVER-57340 and SERVER-57342
                    tasserted(5734001, "TypefDensify should not be kPartition");
                }
            }
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
            if (currentDoc.getNestedField(_path).missing()) {
                // The densify field is not present, let document pass unmodified.
                return nextDoc;
            }
            double val = getDensifyDouble(currentDoc, _path);

            switch (_densifyType) {
                case TypeOfDensify::kFull: {
                    _rangeMin = *_rangeMin;
                    _rangeMax = val;
                    return handleNeedGenFull(currentDoc);
                }
                case TypeOfDensify::kExplicitRange: {
                    return handleNeedGenExplicit(nextDoc.getDocument(), val);
                }
                case TypeOfDensify::kPartition: {
                    // TODO SERVER-57340 and SERVER-57342
                    tasserted(5734003, "TypefDensify should not be kPartition");
                }
            }
        }
        case DensifyState::kHaveGenerator: {
            tassert(5733203,
                    "Densify state is kHaveGenerator but DocGenerator is null or done.",
                    _docGenerator && !_docGenerator->done());

            auto generatedDoc = _docGenerator->getNextDocument();

            switch (_densifyType) {
                case TypeOfDensify::kFull: {
                    if (_docGenerator->done()) {
                        _docGenerator = boost::none;
                        _densifyState = DensifyState::kNeedGen;
                    }
                    _rangeMin = getDensifyDouble(generatedDoc, _path);
                    return generatedDoc;
                }
                case TypeOfDensify::kExplicitRange: {
                    auto val = getDensifyDouble(generatedDoc, _path);
                    // Only want to update the rangeMin if the value - current is divisible by the
                    // step.
                    auto rem = valOffsetFromStep(val, stdx::get<double>(*_rangeMin), _step.step);
                    if (rem == 0) {
                        _rangeMin = val;
                    }
                    resetDocGen();
                    return generatedDoc;
                }
                case TypeOfDensify::kPartition: {
                    // TODO SERVER-57340 and SERVER-57342
                    tasserted(5734004, "TypefDensify should not be kPartition");
                }
            }
        }
        case DensifyState::kDensifyDone: {
            // In the full range, this should only return EOF.
            /** In the explicit range we finish densifying over the range
             and any remaining documents is passed to the next stage */
            auto doc = pSource->getNext();
            if (_densifyType == TypeOfDensify::kFull) {
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
