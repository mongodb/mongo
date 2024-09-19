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

#include <boost/smart_ptr.hpp>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_densify_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/memory_usage_tracker.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"


namespace mongo {

class RangeStatement;
class DensifyValue {
public:
    // Delegate to the zero-argument constructor for std::variant<T>. This constructor is needed
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
        return visit(OverloadedVisitor{[&](Value unwrappedVal) { return unwrappedVal; },
                                       [&](Date_t dateVal) {
                                           return Value(dateVal);
                                       }

                     },
                     _value);
    }

    /**
     * Compare two DensifyValues using the standard comparator convention, returning the sign
     * of (lhs - rhs). Returns -1 if lhs < rhs, 0 if lhs == rhs, and 1 if lhs > rhs.
     */
    static int compare(const DensifyValue& lhs, const DensifyValue& rhs) {
        return visit(OverloadedVisitor{[&](Value lhsVal) {
                                           Value rhsVal = get<Value>(rhs._value);
                                           return Value::compare(lhsVal, rhsVal, nullptr);
                                       },
                                       [&](Date_t lhsVal) {
                                           Date_t rhsVal = get<Date_t>(rhs._value);
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
    static DensifyValue getFromValue(const Value& val) {
        uassert(5733201,
                "Densify field type must be numeric or a date",
                val.numeric() || val.getType() == BSONType::Date);
        if (!val.numeric()) {
            return val.getDate();
        }
        return val;
    }
    static DensifyValue getFromDocument(const Document& doc, const FieldPath& path) {
        Value val = doc.getNestedField(path);
        return getFromValue(val);
    }

    std::string toString() const {
        return visit(OverloadedVisitor{[&](Value v) { return v.toString(); },
                                       [&](Date_t d) {
                                           return d.toString();
                                       }},
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
        return visit(OverloadedVisitor{[&](Value v) { return v.getApproximateSize(); },
                                       [&](Date_t d) {
                                           return Value(d).getApproximateSize();
                                       }},
                     _value);
    }

    /**
     * Returns true if this DensifyValue is a date.
     */
    bool isDate() const {
        return holds_alternative<Date_t>(_value);
    }

    /**
     * Returns the DensifyValue as a date.
     */
    Date_t getDate() const {
        tassert(5733701, "DensifyValue must be a date", isDate());
        return get<Date_t>(_value);
    }

    /**
     * Returns true if this DensifyValue is a number.
     */
    bool isNumber() const {
        return holds_alternative<Value>(_value);
    }

    /**
     * Returns the DensifyValue as a number in a Value.
     */
    Value getNumber() const {
        tassert(5733702, "DensifyValue must be a number", isNumber());
        return get<Value>(_value);
    }

    /**
     * Returns true if this DensifyValue and the other one are both the same underlying type.
     */
    bool isSameTypeAs(const DensifyValue& other) const {
        return (this->isDate() && other.isDate()) || (this->isNumber() && other.isNumber());
    }

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
    std::variant<Value, Date_t> _value;
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
    using Bounds = std::variant<Full, Partition, ExplicitBounds>;

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

    Value serialize(const SerializationOptions& opts) const {
        MutableDocument spec;
        spec[kArgStep] = opts.serializeLiteral(_step);
        spec[kArgBounds] =
            visit(OverloadedVisitor{[&](Full) { return Value(kValFull); },
                                    [&](Partition) { return Value(kValPartition); },
                                    [&](ExplicitBounds bounds) {
                                        return Value(std::vector<Value>(
                                            {opts.serializeLiteral(bounds.first.toValue()),
                                             opts.serializeLiteral(bounds.second.toValue())}));
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
                                  FieldPath field,
                                  std::list<FieldPath> partitions,
                                  RangeStatement range)
        : DocumentSource(kStageName, pExpCtx),
          _memTracker(internalDocumentSourceDensifyMaxMemoryBytes.load()),
          _field(std::move(field)),
          _partitions(std::move(partitions)),
          _range(std::move(range)),
          _partitionTable(pExpCtx->getValueComparator()
                              .makeUnorderedValueMap<SimpleMemoryUsageTokenWith<DensifyValue>>()) {
        _maxDocs = internalQueryMaxAllowedDensifyDocs.load();
    };

    class DocGenerator {
    public:
        DocGenerator(DensifyValue current,
                     RangeStatement range,
                     FieldPath fieldName,
                     boost::optional<Document> includeFields,
                     boost::optional<Document> finalDoc,
                     boost::optional<Value> partitionKey,
                     ValueComparator comp,
                     size_t* counter,
                     bool maxInclusive);
        Document getNextDocument();
        bool done() const;
        // Helper to return whether this is the last generated document. This is useful because
        // the last generated document is always on the step. Expected to be called after generating
        // a document to describe the document that was just generated as 'last' or 'not last'.
        bool lastGeneratedDoc() const;


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
        // The partition key for all documents created by this generator.
        boost::optional<Value> _partitionKey;
        // The minimum value that this generator will create, therefore the next generated
        // document will have this value.
        DensifyValue _min;
        bool _maxInclusive = false;

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

    DocumentSourceType getType() const override {
        return DocumentSourceType::kInternalDensify;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert(_field.fullPath());
        // We don't need to traverse _partitionExpr because it was generated from _partitions.
        // Every ExpressionFieldPath it contains is already covered by _partitions.
        for (const auto& field : _partitions) {
            deps->fields.insert(field.fullPath());
        }
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        // The partition expression cannot refer to any variables because it is internally generated
        // based on a set of field paths.
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

    /**
     * Helper to pull a document from the previous stage and verify that it is eligible for
     * densification. Returns a tuple of <Return immediately, pulled document, value to be
     * densified, partition key>.
     */
    std::tuple<bool, DocumentSource::GetNextResult, DensifyValue, Value> getAndCheckInvalidDoc();

    void assertDensifyType(DensifyValue val) {
        uassert(6053600,
                val.isNumber()
                    ? "Encountered numeric densify value in collection when step has a date unit."
                    : "Encountered date densify value in collection when step does not have a date "
                      "unit.",
                (!_range.getUnit() && val.isNumber()) || (_range.getUnit() && val.isDate()));
    }

    DensifyValue getDensifyValue(const Document& doc) {
        auto val = DensifyValue::getFromDocument(doc, _field);
        assertDensifyType(val);
        return val;
    }

    DensifyValue getDensifyValue(const Value& val) {
        auto densifyVal = DensifyValue::getFromValue(val);
        assertDensifyType(densifyVal);
        return densifyVal;
    }

    Value getDensifyPartition(const Document& doc) {
        auto part = _partitionExpr->evaluate(doc, &pExpCtx->variables);
        return part;
    }

    /**
     * Decides whether or not to build a DocGen and return the first document generated or return
     * the current doc if the rangeMin + step is greater than rangeMax.
     * Optionally include the densify and partition values for the generator if they've already
     * been calculated.
     */
    DocumentSource::GetNextResult handleNeedGen(Document currentDoc,
                                                DensifyValue lastSeen,
                                                DensifyValue& densifyVal,
                                                Value& partitinKey);

    /**
     * Check whether or not the first document in the partition needs to be densified. Returns the
     * document this iteration should return.
     */
    DocumentSource::GetNextResult checkFirstDocAgainstRangeStart(Document doc,
                                                                 DensifyValue& densifyVal,
                                                                 Value& partitionKey);

    /**
     * Handles when the pSource has been exhausted. Has different behavior depending on the densify
     * mode, but generally speaking sets the min/max for what still needs to be done, and then
     * delegates to helpers to finish densifying each partition individually. In the non-partitioned
     * case, this is the "trivial" partition of the whole collection.
     */
    DocumentSource::GetNextResult handleSourceExhausted();

    /**
     * Set up necessary tracking variables based on the densify mode. Only called once at the
     * beginning of execution.
     */
    void initializeState();

    /**
     * Handles building a document generator once we've seen an EOF for partitioned input. Min will
     * be the last seen value in the partition unless it is less than the optional 'minOverride'.
     * Helper is to share code between visit functions.
     */
    DocumentSource::GetNextResult finishDensifyingPartitionedInputHelper(
        DensifyValue max,
        boost::optional<DensifyValue> minOverride = boost::none,
        bool maxInclusive = false);

    boost::optional<Document> createIncludeFieldsObj(Value partitionKey);
    /**
     * Helper to set the value in the partition table.
     */
    void setPartitionValue(DensifyValue partitionVal, Value partitionKey) {
        SimpleMemoryUsageToken memoryToken{
            partitionKey.getApproximateSize() + partitionVal.getApproximateSize(), &_memTracker};
        _partitionTable[partitionKey] = {std::move(memoryToken), std::move(partitionVal)};
        uassert(6007200,
                str::stream() << "$densify exceeded memory limit of "
                              << _memTracker.maxAllowedMemoryUsageBytes(),
                _memTracker.withinMemoryLimit());
    }

    void setPartitionValue(Document doc,
                           boost::optional<DensifyValue> valueOverride = boost::none) {
        tassert(8246103, "partitionExpr", _partitionExpr);
        auto partitionKey = getDensifyPartition(doc);
        auto partitionVal = valueOverride ? *valueOverride : getDensifyValue(doc);
        setPartitionValue(partitionVal, partitionKey);
    }

    /**
     * Create a document generator for the given range statement. The generation will start at 'min'
     * (inclusive) and will go to the end of the given 'range'. Whether or not a document at the
     * range maximum depends on 'maxInclusive' -- if true, the range will be inclusive on both ends.
     * Will output documents that include any given 'includeFields' (with their values) and, if
     * given, will output 'finalDoc' unchanged at the end of the generated documents.
     * Pass the partition key to the generator to avoid having to compute it for each document
     * the generator creates.
     */
    void createDocGenerator(DensifyValue min,
                            RangeStatement range,
                            boost::optional<Document> includeFields,
                            boost::optional<Document> finalDoc,
                            boost::optional<Value> partitionKey,
                            bool maxInclusive = false) {
        _docGenerator = DocGenerator(min,
                                     range,
                                     _field,
                                     includeFields,
                                     finalDoc,
                                     partitionKey,
                                     pExpCtx->getValueComparator(),
                                     &_docsGenerated,
                                     maxInclusive);
    }
    void createDocGenerator(DensifyValue min, RangeStatement range) {
        createDocGenerator(min, range, boost::none, boost::none, boost::none);
    }


    Pipeline::SourceContainer::iterator combineSorts(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container);

    boost::optional<DocGenerator> _docGenerator = boost::none;

    // Used to keep track of the bounds for densification.
    // Track the minimum seen across the input set. Should be the first document seen.
    boost::optional<DensifyValue> _fullDensifyGlobalMin = boost::none;
    // Track the maximum seen across the input set. Should be the last document seen.
    boost::optional<DensifyValue> _fullDensifyGlobalMax = boost::none;
    bool _isFullDensify = false;
    // Value to store the beginning/end of the densification range. Note that these may also be
    // stored in the range object we had parsed initially, but we store them here again for easier
    // access and to avoid using parsing objects during execution.
    boost::optional<DensifyValue> _rangeDensifyStart = boost::none;
    boost::optional<DensifyValue> _rangeDensifyEnd = boost::none;

    // _partitionExpr has two purposes:
    // 1. to determine which partition a document belongs in.
    // 2. to initialize new documents with the right partition key.
    // For example, if the stage had 'partitionByFields: ["a", "x.y"]' then this expression
    // would be {a: "$a", {x: {y: "$x.y"}}}.
    // In the non-partitioned case, this is set to be a constant expression "true".
    boost::intrusive_ptr<Expression> _partitionExpr;

    enum class DensifyState {
        kUninitialized,
        kNeedGen,
        kHaveGenerator,
        kFinishingDensifyNoGenerator,
        kFinishingDensifyWithGenerator,
        kDensifyDone
    };

    // Keep track of documents generated, error if it goes above the limit.
    size_t _docsGenerated = 0;
    size_t _maxDocs = 0;
    SimpleMemoryUsageTracker _memTracker;

    DensifyState _densifyState = DensifyState::kUninitialized;
    // The field on which we are densifying.
    FieldPath _field;
    // The list of partitions we are using to densify taken from the original stage object.
    std::list<FieldPath> _partitions;
    // Range statement taken from the original stage object.
    RangeStatement _range;
    // Store of the value we've seen for each partition. In the non-partitioned case should only
    // have one key -- "true". This allows us to pretend that all input is partitioned and use the
    // same codepath.
    ValueUnorderedMap<SimpleMemoryUsageTokenWith<DensifyValue>> _partitionTable;
};
}  // namespace mongo
