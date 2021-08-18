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

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_densify_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/time_support.h"
#include "mongo/util/visit_helper.h"

namespace mongo {

class RangeStatement {
public:
    static constexpr StringData kArgUnit = "unit"_sd;
    static constexpr StringData kArgBounds = "bounds"_sd;
    static constexpr StringData kArgStep = "step"_sd;

    static constexpr StringData kValFull = "full"_sd;
    static constexpr StringData kValPartition = "partition"_sd;

    struct Full {};
    struct Partition {};
    typedef std::pair<Date_t, Date_t> DateBounds;
    typedef std::pair<Value, Value> NumericBounds;
    using Bounds = stdx::variant<Full, Partition, DateBounds, NumericBounds>;

    Bounds getBounds() {
        return _bounds;
    }

    boost::optional<TimeUnit> getUnit() {
        return _unit;
    }

    Value getStep() {
        return _step;
    }

    RangeStatement(Value step, Bounds bounds, boost::optional<TimeUnit> unit)
        : _step(step), _bounds(bounds), _unit(unit) {}

    static RangeStatement parse(RangeSpec spec);

    Value serialize() const {
        MutableDocument spec;
        spec[kArgStep] = _step;
        spec[kArgBounds] = stdx::visit(
            visit_helper::Overloaded{[&](Full full) { return Value(kValFull); },
                                     [&](Partition partition) { return Value(kValPartition); },
                                     [&](std::pair<Date_t, Date_t> dates) {
                                         return Value({Value(dates.first), Value(dates.second)});
                                     },
                                     [&](std::pair<Value, Value> vals) {
                                         return Value({vals.first, vals.second});
                                     }},
            _bounds);
        if (_unit)
            spec[kArgUnit] = Value(serializeTimeUnit(*_unit));
        return spec.freezeToValue();
    }

private:
    Value _step;
    Bounds _bounds;
    boost::optional<TimeUnit> _unit = boost::none;
};

namespace document_source_densify {
constexpr StringData kStageName = "$densify"_sd;

/**
 * The 'internal' parameter specifies whether or not we create a sort stage that is required for
 * correct execution of an _internalDensify stage.
 */
std::list<boost::intrusive_ptr<DocumentSource>> createFromBsonInternal(
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    StringData stageName,
    bool isInternal);
std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

/**
 * The 'internal' parameter specifies whether or not we create a sort stage that is required for
 * correct execution of an _internalDensify stage.
 */
std::list<boost::intrusive_ptr<DocumentSource>> create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::list<FieldPath> partitions,
    FieldPath field,
    RangeStatement rangeStatement,
    bool isInternal);
}  // namespace document_source_densify

class DocumentSourceInternalDensify final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalDensify"_sd;
    static constexpr StringData kPartitionByFieldsFieldName = "partitionByFields"_sd;
    static constexpr StringData kFieldFieldName = "field"_sd;
    static constexpr StringData kRangeFieldName = "range"_sd;

    using DensifyValueType = stdx::variant<Value, Date_t>;

    DocumentSourceInternalDensify(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  const FieldPath& field,
                                  const std::list<FieldPath>& partitions,
                                  const RangeStatement& range)
        : DocumentSource(kStageName, pExpCtx),
          _field(std::move(field)),
          _partitions(std::move(partitions)),
          _range(std::move(range)) {
        _current = stdx::visit(
            visit_helper::Overloaded{
                [&](RangeStatement::Full full) -> boost::optional<DensifyValueType> {
                    return boost::none;
                },
                [&](RangeStatement::Partition partition) -> boost::optional<DensifyValueType> {
                    return boost::none;
                },
                [&](RangeStatement::DateBounds bounds) -> boost::optional<DensifyValueType> {
                    return DensifyValueType(bounds.first);
                },
                [&](RangeStatement::NumericBounds bounds) -> boost::optional<DensifyValueType> {
                    return DensifyValueType(bounds.first);
                }},
            _range.getBounds());
    };

    class DocGenerator {
    public:
        DocGenerator(DensifyValueType current,
                     RangeStatement range,
                     FieldPath fieldName,
                     boost::optional<Document> includeFields,
                     boost::optional<Document> finalDoc,
                     ValueComparator comp);
        Document getNextDocument();
        bool done() const;

    private:
        ValueComparator _comp;
        RangeStatement _range;
        // The field to add to 'includeFields' to generate a document.
        FieldPath _path;
        Document _includeFields;
        // The document that is equal to or larger than the upper bound that prompted the creation
        // of this generator. Will be returned after the final generated document. Can be
        // boost::none if we are generating the values at the end of the range.
        boost::optional<Document> _finalDoc;
        // The minimum value that this generator will create, therefore the next generated document
        // will have this value.
        DensifyValueType _min;

        enum class GeneratorState {
            // Generating documents between '_min' and the upper bound.
            kGeneratingDocuments,
            // Generated all necessary documents, waiting for a final 'getNextDocument()' call.
            kReturningFinalDocument,
            kDone,
        };

        GeneratorState _state = GeneratorState::kGeneratingDocuments;
    };

    DocumentSourceInternalDensify(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        double step,
        FieldPath path,
        boost::optional<std::pair<DensifyValueType, DensifyValueType>> range = boost::none);

    static boost::intrusive_ptr<DocumentSourceInternalDensify> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::SEE_NEXT;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    GetNextResult doGetNext() final;

private:
    enum class ValComparedToRange {
        kBelow,
        kRangeMin,
        kInside,
        kAbove,
    };

    bool compareValues(Value::DeferredComparison deferredComparison) {
        return pExpCtx->getValueComparator().evaluate(deferredComparison);
    }

    int compareValues(const Value& lhs, const Value& rhs) {
        return pExpCtx->getValueComparator().compare(lhs, rhs);
    }

    /**
     * Decides whether or not to build a DocGen and return the first document generated or return
     * the current doc if the rangeMin + step is greater than rangeMax.
     */
    DocumentSource::GetNextResult handleNeedGenFull(Document currentDoc, Value max);

    /**
     * Checks where the current doc's value lies compared to the range and creates the correct
     * DocGen if needed and returns the next doc.
     */
    DocumentSource::GetNextResult handleNeedGenExplicit(Document currentDoc,
                                                        Value val,
                                                        RangeStatement::NumericBounds bounds);

    /**
     * Takes care of when an EOF has been hit for the explicit case. It checks if we have finished
     * densifying over the range, and if so changes the state to be kDensify done. Otherwise it
     * builds a new generator that will finish densifying over the range and changes the state to
     * kHaveGen.
     */
    DocumentSource::GetNextResult densifyAfterEOF(RangeStatement::NumericBounds);

    /**
     * Creates a document generator based on the value passed in, the current _current, and the
     * NumericBounds. Once created, the state changes to kHaveGenerator and the first document from
     * the generator is returned.
     */
    DocumentSource::GetNextResult processDocAboveMinBound(Value val,
                                                          RangeStatement::NumericBounds bounds,
                                                          Document doc);

    /**
     * Takes in a value and checks if the value is below, on the bottom, inside, or above the
     * range, and returns the equivelant state from ValComparedToRange.
     */
    ValComparedToRange processRange(Value val, Value current, RangeStatement::NumericBounds bounds);

    /**
     * Handles when the pSource has been exhausted. In the full case we are done with the
     * densification process and the state becomes kDensifyDone, however in the explicit case we may
     * still need to densify over the remainder of the range, so the densifyAfterEOF() function is
     * called.
     */
    DocumentSource::GetNextResult handleSourceExhausted();

    /**
     * Checks if the current document generator is done. If it is and we have finished densifying,
     * it changes the state to be kDensifyDone. If there is more to densify, the state becomes
     * kNeedGen. The generator is also deleted.
     */
    void resetDocGen(RangeStatement::NumericBounds bounds);

    auto valOffsetFromStep(Value val, Value sub, Value step) {
        Value diff = uassertStatusOK(ExpressionSubtract::apply(val, sub));
        return uassertStatusOK(ExpressionMod::apply(diff, step));
    }
    boost::optional<DocGenerator> _docGenerator = boost::none;

    /**
     * The minimum value that the document generator will create, therefore the next generated
     * document will have this value. This is also used as last seen value by the explicit case.
     */
    boost::optional<DensifyValueType> _current = boost::none;

    bool _eof = false;

    enum class DensifyState { kUninitializedOrBelowRange, kNeedGen, kHaveGenerator, kDensifyDone };

    enum class TypeOfDensify {
        kFull,
        kExplicitRange,
        kPartition,
    };

    DensifyState _densifyState = DensifyState::kUninitializedOrBelowRange;
    TypeOfDensify _densifyType;
    FieldPath _field;
    std::list<FieldPath> _partitions;
    RangeStatement _range;
};
}  // namespace mongo
