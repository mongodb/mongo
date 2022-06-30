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
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_densify_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/memory_usage_tracker.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/time_support.h"


namespace mongo {

class RangeStatement;
class DensifyValue {
public:
    // Delegate to the zero-argument constructor for stdx::variant<T>. This constructor is needed
    // for DensifyValue to be able to be the value type in a ValueUnorderedMap.
    DensifyValue() : _value() {}
    DensifyValue(Value val) : _value(val) {}
    DensifyValue(Date_t date) : _value(date) {}

    TimeZone timezone() const {
        return TimeZoneDatabase::utcZone();
    }

    /**
     * Convert a DensifyValue into a Value for use in documents/serialization.
     */
    Value toValue() const {
        return stdx::visit(OverloadedVisitor{[&](Value unwrappedVal) { return unwrappedVal; },
                                             [&](Date_t dateVal) { return Value(dateVal); }

                           },
                           _value);
    }

    /**
     * Compare two DensifyValues using the standard comparator convention, returning the sign
     * of (lhs - rhs). Returns -1 if lhs < rhs, 0 if lhs == rhs, and 1 if lhs > rhs.
     */
    static int compare(const DensifyValue& lhs, const DensifyValue& rhs) {
        return stdx::visit(OverloadedVisitor{[&](Value lhsVal) {
                                                 Value rhsVal = stdx::get<Value>(rhs._value);
                                                 return Value::compare(lhsVal, rhsVal, nullptr);
                                             },
                                             [&](Date_t lhsVal) {
                                                 Date_t rhsVal = stdx::get<Date_t>(rhs._value);
                                                 return Value::compare(
                                                     Value(lhsVal), Value(rhsVal), nullptr);
                                             }},
                           lhs._value);
    }

    /**
     * Get the value to be densified from a document. The function checks the type to ensure that
     * the document has either a numeric value or a date in the proper field, and throws an error
     * otherwise.
     */
    static DensifyValue getFromDocument(const Document& doc, const FieldPath& path) {
        Value val = doc.getNestedField(path);
        uassert(5733201,
                "Densify field type must be numeric or a date",
                val.numeric() || val.getType() == BSONType::Date);
        if (!val.numeric()) {
            return val.getDate();
        }
        return val;
    }

    std::string toString() const {
        return stdx::visit(OverloadedVisitor{[&](Value v) { return v.toString(); },
                                             [&](Date_t d) { return d.toString(); }},
                           _value);
    }

    /**
     * Returns a new DensifyValue incremented by the step in the provided range.
     */
    DensifyValue increment(const RangeStatement& range) const;

    /**
     * Returns a new DensifyValue decremented by the step in the provided range.
     */
    DensifyValue decrement(const RangeStatement& range) const;

    /**
     * Delegate to Value::getApproximateSize().
     */
    size_t getApproximateSize() const {
        return stdx::visit(
            OverloadedVisitor{[&](Value v) { return v.getApproximateSize(); },
                              [&](Date_t d) { return Value(d).getApproximateSize(); }},
            _value);
    }

    /**
     * Returns true if this DensifyValue is a date.
     */
    bool isDate() const {
        return stdx::holds_alternative<Date_t>(_value);
    }

    /**
     * Returns the DensifyValue as a date.
     */
    Date_t getDate() const {
        tassert(5733701, "DensifyValue must be a date", isDate());
        return stdx::get<Date_t>(_value);
    }

    /**
     * Returns true if this DensifyValue is a number.
     */
    bool isNumber() const {
        return stdx::holds_alternative<Value>(_value);
    }

    /**
     * Returns the DensifyValue as a number in a Value.
     */
    Value getNumber() const {
        tassert(5733702, "DensifyValue must be a number", isNumber());
        return stdx::get<Value>(_value);
    }

    /**
     * Returns true if this DensifyValue and the other one are both the same underlying type.
     */
    bool isSameTypeAs(const DensifyValue& other) const {
        return (this->isDate() && other.isDate()) || (this->isNumber() && other.isNumber());
    }

    /**
     * Checks if the value is exactly on the step defined in the given RangeStatement
     * relative to the provided base value.
     */
    bool isOnStepRelativeTo(DensifyValue base, RangeStatement range) const;

    /**
     * Comparison operator overloads.
     */
    bool operator==(const DensifyValue& rhs) const {
        return compare(*this, rhs) == 0;
    }

    bool operator!=(const DensifyValue& rhs) const {
        return !(*this == rhs);
    }

    bool operator<(const DensifyValue& rhs) const {
        return compare(*this, rhs) < 0;
    }

    bool operator>(const DensifyValue& rhs) const {
        return compare(*this, rhs) > 0;
    }

    bool operator>=(const DensifyValue& rhs) const {
        return !(*this < rhs);
    }

    bool operator<=(const DensifyValue& rhs) const {
        return !(*this > rhs);
    }

    /**
     * Stream pipe operator for debug purposes.
     */
    friend std::ostream& operator<<(std::ostream& os, const DensifyValue& val) {
        os << val.toString();
        return os;
    }

private:
    stdx::variant<Value, Date_t> _value;
};
class RangeStatement {
public:
    static constexpr StringData kArgUnit = "unit"_sd;
    static constexpr StringData kArgBounds = "bounds"_sd;
    static constexpr StringData kArgStep = "step"_sd;

    static constexpr StringData kValFull = "full"_sd;
    static constexpr StringData kValPartition = "partition"_sd;

    struct Full {};
    struct Partition {};
    typedef std::pair<DensifyValue, DensifyValue> ExplicitBounds;
    using Bounds = stdx::variant<Full, Partition, ExplicitBounds>;

    Bounds getBounds() const {
        return _bounds;
    }

    boost::optional<TimeUnit> getUnit() const {
        return _unit;
    }

    Value getStep() const {
        return _step;
    }

    RangeStatement(Value step, Bounds bounds, boost::optional<TimeUnit> unit)
        : _step(step), _bounds(bounds), _unit(unit) {}

    static RangeStatement parse(RangeSpec spec);

    Value serialize() const {
        MutableDocument spec;
        spec[kArgStep] = _step;
        spec[kArgBounds] = stdx::visit(
            OverloadedVisitor{[&](Full) { return Value(kValFull); },
                              [&](Partition) { return Value(kValPartition); },
                              [&](ExplicitBounds bounds) {
                                  return Value(std::vector<Value>(
                                      {bounds.first.toValue(), bounds.second.toValue()}));
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

    DocumentSourceInternalDensify(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  const FieldPath& field,
                                  const std::list<FieldPath>& partitions,
                                  const RangeStatement& range)
        : DocumentSource(kStageName, pExpCtx),
          _field(std::move(field)),
          _partitions(std::move(partitions)),
          _range(std::move(range)),
          _partitionTable(pExpCtx->getValueComparator().makeUnorderedValueMap<DensifyValue>()),
          _memTracker(
              MemoryUsageTracker(false, internalDocumentSourceDensifyMaxMemoryBytes.load())) {
        _maxDocs = internalQueryMaxAllowedDensifyDocs.load();
    };

    class DocGenerator {
    public:
        DocGenerator(DensifyValue current,
                     RangeStatement range,
                     FieldPath fieldName,
                     boost::optional<Document> includeFields,
                     boost::optional<Document> finalDoc,
                     ValueComparator comp,
                     size_t* counter);
        Document getNextDocument();
        bool done() const;


    private:
        ValueComparator _comp;
        RangeStatement _range;
        // The field to add to 'includeFields' to generate a document.
        FieldPath _path;
        Document _includeFields;
        // The document that is equal to or larger than the upper bound that prompted the
        // creation of this generator. Will be returned after the final generated document. Can
        // be boost::none if we are generating the values at the end of the range.
        boost::optional<Document> _finalDoc;
        // The minimum value that this generator will create, therefore the next generated
        // document will have this value.
        DensifyValue _min;

        enum class GeneratorState {
            // Generating documents between '_min' and the upper bound.
            kGeneratingDocuments,
            // Generated all necessary documents, waiting for a final 'getNextDocument()' call.
            kReturningFinalDocument,
            kDone,
        };

        GeneratorState _state = GeneratorState::kGeneratingDocuments;
        // Value to increment when returning a generated document. This is a pointer to the counter
        // that keeps track of the total number of documents generated by the owning stage across
        // all generators.
        size_t* _counter;
    };

    DocumentSourceInternalDensify(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        double step,
        FieldPath path,
        boost::optional<std::pair<DensifyValue, DensifyValue>> range = boost::none);

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
        deps->fields.insert(_field.fullPath());
        // We don't need to traverse _partitionExpr because it was generated from _partitions.
        // Every ExpressionFieldPath it contains is already covered by _partitions.
        for (const auto& field : _partitions) {
            deps->fields.insert(field.fullPath());
        }
        return DepsTracker::State::SEE_NEXT;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    GetNextResult doGetNext() final;

protected:
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

private:
    enum class ValComparedToRange {
        kBelow,
        kRangeMin,
        kInside,
        kAbove,
    };

    DensifyValue getDensifyValue(const Document& doc) {
        auto val = DensifyValue::getFromDocument(doc, _field);
        uassert(6053600,
                val.isNumber()
                    ? "Encountered numeric densify value in collection when step has a date unit."
                    : "Encountered date densify value in collection when step does not have a date "
                      "unit.",
                (!_range.getUnit() && val.isNumber()) || (_range.getUnit() && val.isDate()));
        return val;
    }

    Value getDensifyPartition(const Document& doc) {
        auto part = _partitionExpr->evaluate(doc, &pExpCtx->variables);
        return part;
    }

    /**
     * Decides whether or not to build a DocGen and return the first document generated or return
     * the current doc if the rangeMin + step is greater than rangeMax. Used for both 'full' and
     * 'partition' bounds.
     */
    DocumentSource::GetNextResult handleNeedGen(Document currentDoc);

    /**
     * Checks where the current doc's value lies compared to the range and creates the correct
     * DocGen if needed and returns the next doc.
     */
    DocumentSource::GetNextResult handleNeedGenExplicit(Document currentDoc);

    /**
     * Takes care of when an EOF has been hit for the explicit case. It checks if we have finished
     * densifying over the range, and if so changes the state to be kDensify done. Otherwise it
     * builds a new generator that will finish densifying over the range and changes the state to
     * kHaveGen. Only used if the input is not partitioned.
     */
    DocumentSource::GetNextResult densifyExplicitRangeAfterEOF();

    /**
     * Decide what to do for the first document in a given partition for explicit range. Either
     * generate documents between the minimum and the value, or just return it.
     */
    DocumentSource::GetNextResult processFirstDocForExplicitRange(Document doc);

    /**
     * Creates a document generator based on the value passed in, the current _current, and the
     * ExplicitBounds on the stage. Once created, the state changes to kHaveGenerator and the first
     * document from the generator is returned.
     */
    DocumentSource::GetNextResult processDocAboveExplicitMinBound(Document doc);

    /**
     * Takes in a value and checks if the value is below, on the bottom, inside, or above the
     * range, and returns the equivelant state from ValComparedToRange.
     */
    ValComparedToRange getPositionRelativeToRange(DensifyValue val);

    /**
     * Handles when the pSource has been exhausted. In the full case we are done with the
     * densification process and the state becomes kDensifyDone, however in the explicit case we
     * may still need to densify over the remainder of the range, so the
     * densifyExplicitRangeAfterEOF() function is called.
     */
    DocumentSource::GetNextResult handleSourceExhausted();

    /**
     * Handles building a document generator once we've seen an EOF for partitioned input. Min will
     * be the last seen value in the partition unless it is less than the optional 'minOverride'.
     * Helper is to share code between visit functions.
     */
    DocumentSource::GetNextResult finishDensifyingPartitionedInput();
    DocumentSource::GetNextResult finishDensifyingPartitionedInputHelper(
        DensifyValue max, boost::optional<DensifyValue> minOverride = boost::none);

    /**
     * Checks if the current document generator is done. If it is and we have finished densifying,
     * it changes the state to be kDensifyDone. If there is more to densify, the state becomes
     * kNeedGen. The generator is also deleted.
     */
    void resetDocGen(RangeStatement::ExplicitBounds bounds);

    /**
     * Set up the state for densifying over partitions.
     */
    void initializePartitionState(Document initialDoc);

    /**
     * Helper to set the value in the partition table.
     */
    void setPartitionValue(Document doc) {
        if (_partitionExpr) {
            auto partitionKey = getDensifyPartition(doc);
            auto partitionVal = getDensifyValue(doc);
            auto lastValForPartitionIt = _partitionTable.find(partitionKey);
            if (lastValForPartitionIt == _partitionTable.end()) {
                // If this is a new partition, store the size of the key and the value.
                _memTracker.update(partitionKey.getApproximateSize() +
                                   partitionVal.getApproximateSize());
            } else {
                // Subtract the size of the previous value and add the new one.
                _memTracker.update(partitionVal.getApproximateSize() -
                                   lastValForPartitionIt->second.getApproximateSize());
            }
            uassert(6007200,
                    str::stream() << "$densify exceeded memory limit of "
                                  << _memTracker._maxAllowedMemoryUsageBytes,
                    _memTracker.withinMemoryLimit());

            _partitionTable[partitionKey] = partitionVal;
        }
    }

    /**
     * Helpers to create doc generators. Sets _docGenerator to the created generator.
     */
    void createDocGenerator(DensifyValue min,
                            RangeStatement range,
                            boost::optional<Document> includeFields,
                            boost::optional<Document> finalDoc) {
        _docGenerator = DocGenerator(min,
                                     range,
                                     _field,
                                     includeFields,
                                     finalDoc,
                                     pExpCtx->getValueComparator(),
                                     &_docsGenerated);
    }
    void createDocGenerator(DensifyValue min, RangeStatement range) {
        createDocGenerator(min, range, boost::none, boost::none);
    }


    Pipeline::SourceContainer::iterator combineSorts(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container);

    boost::optional<DocGenerator> _docGenerator = boost::none;

    /**
     * The last value seen or generated by the stage that is also in line with the step.
     */
    boost::optional<DensifyValue> _current = boost::none;

    // Used to keep track of the bounds for densification in the full case.
    boost::optional<DensifyValue> _globalMin = boost::none;
    boost::optional<DensifyValue> _globalMax = boost::none;

    // _partitionExpr has two purposes:
    // 1. to determine which partition a document belongs in.
    // 2. to initialize new documents with the right partition key.
    // For example, if the stage had 'partitionByFields: ["a", "x.y"]' then this expression
    // would be {a: "$a", {x: {y: "$x.y"}}}.
    boost::intrusive_ptr<ExpressionObject> _partitionExpr;

    bool _eof = false;

    enum class DensifyState {
        kUninitializedOrBelowRange,
        kNeedGen,
        kHaveGenerator,
        kFinishingDensify,
        kDensifyDone
    };

    DensifyState _densifyState = DensifyState::kUninitializedOrBelowRange;
    FieldPath _field;
    std::list<FieldPath> _partitions;
    RangeStatement _range;
    // Store of the value we've seen for each partition.
    ValueUnorderedMap<DensifyValue> _partitionTable;

    // Keep track of documents generated, error if it goes above the limit.
    size_t _docsGenerated = 0;
    size_t _maxDocs = 0;
    MemoryUsageTracker _memTracker;
};
}  // namespace mongo
