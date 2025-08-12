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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_densify_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"

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
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


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
                val.numeric() || val.getType() == BSONType::date);
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
          _field(std::move(field)),
          _partitions(std::move(partitions)),
          _range(std::move(range)) {};

    DocumentSourceInternalDensify(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        double step,
        FieldPath path,
        boost::optional<std::pair<DensifyValue, DensifyValue>> range = boost::none);

    static boost::intrusive_ptr<DocumentSourceInternalDensify> create(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
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
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
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

protected:
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalDensifyToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceContainer::iterator combineSorts(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container);

    // The field on which we are densifying.
    FieldPath _field;
    // The list of partitions we are using to densify taken from the original stage object.
    std::list<FieldPath> _partitions;
    // Range statement taken from the original stage object.
    RangeStatement _range;
};
}  // namespace mongo
