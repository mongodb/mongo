/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/accumulator_for_bucket_auto.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <iterator>
#include <map>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

namespace mongo {
using FirstLastSense = AccumulatorFirstLastN::Sense;
using FactoryFnMap = StringMap<std::function<AccumulatorState::Factory(ExpressionContext* const)>>;

/**
 * The custom positional accumulators ($first, $firstN, $last, $lastN) for $bucketAuto stage.
 * Because $bucketAuto stage sorts the documents for determining the bucket boundaries, it has to do
 * some extra work to remember the docs' original positions. It wraps the original documents under
 * the field 'AccumulatorN::kFieldNameOutput' and sets their positions under
 * 'AccumulatorN::kFieldNameGeneratedSortKey', requiring custom accumulators to unwrap those
 * documents.
 *
 * The custom accumulators are designed to consume the wrapped documents and determine the
 * accumulated results based on their original positions. The mechanism is similar to
 * $topN/$bottomN, except we can assume the positions are always unique and hence 'map' is used
 * rather than 'multimap'.
 *
 * Note that the serialized output is the same as the original $firstN/$lastN such that the
 * explain outputs are identical to the non-custom version.
 */
template <AccumulatorFirstLastN::Sense sense, bool single>
class AccumulatorFirstLastNForBucketAuto : public AccumulatorN {
public:
    using Sense = AccumulatorFirstLastN::Sense;
    // pair of (sortKey, output) for storing in the internal map.
    using KeyOutPair = std::pair<long long, Value>;

    AccumulatorFirstLastNForBucketAuto(ExpressionContext* expCtx);

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);

    static constexpr StringData getName() {
        if constexpr (single) {
            if constexpr (sense == Sense::kFirst) {
                return "$first"_sd;
            } else {
                return "$last"_sd;
            }
        } else {
            if constexpr (sense == Sense::kFirst) {
                return "$firstN"_sd;
            } else {
                return "$lastN"_sd;
            }
        }
    }

    const char* getOpName() const final;

    AccumulatorType getAccumulatorType() const override {
        if constexpr (sense == Sense::kFirst) {
            return AccumulatorType::kFirstN;
        } else {
            return AccumulatorType::kLastN;
        }
    }

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       const SerializationOptions& options = {}) const final;

    void reset() final;

    ExpressionNary::Associativity getAssociativity() const final {
        tasserted(8533702,
                  str::stream() << "The specialized accumulator for $bucketAuto does not support "
                                   "associativity. Accumulator op name: "
                                << getOpName());
    }

    bool isCommutative() const final {
        tasserted(8533703,
                  str::stream() << "The specialized accumulator for $bucketAuto does not support "
                                   "commutativity. Accumulator op name: "
                                << getOpName());
    }

    Value getValue(bool toBeMerged) final;

private:
    // firstN/lastN do NOT ignore null values.
    void _processValue(const Value& val) final;

    KeyOutPair _genKeyOutPair(const Value& val);

    std::map<long long, SimpleMemoryUsageTokenWith<Value>> _map;
};

namespace {

/**
 * 'factoryFnMap' stores lambdas to create AccumulatorState::Factory instance by binding the
 * parameter 'expCtx' to the returned lambda. This map can also check if a given op name is one of
 * the positional accumulators.
 */
static FactoryFnMap factoryFnMap{
    {"$first",
     [](ExpressionContext* const expCtx) {
         return [expCtx] {
             return AccumulatorFirstLastNForBucketAuto<AccumulatorFirstLastN::Sense::kFirst,
                                                       true>::create(expCtx);
         };
     }},
    {"$firstN",
     [](ExpressionContext* const expCtx) {
         return [expCtx] {
             return AccumulatorFirstLastNForBucketAuto<AccumulatorFirstLastN::Sense::kFirst,
                                                       false>::create(expCtx);
         };
     }},
    {"$last",
     [](ExpressionContext* const expCtx) {
         return [expCtx] {
             return AccumulatorFirstLastNForBucketAuto<AccumulatorFirstLastN::Sense::kLast,
                                                       true>::create(expCtx);
         };
     }},
    {"$lastN",
     [](ExpressionContext* const expCtx) {
         return [expCtx] {
             return AccumulatorFirstLastNForBucketAuto<AccumulatorFirstLastN::Sense::kLast,
                                                       false>::create(expCtx);
         };
     }},
};
}  // namespace

template <FirstLastSense sense, bool single>
AccumulatorFirstLastNForBucketAuto<sense, single>::AccumulatorFirstLastNForBucketAuto(
    ExpressionContext* const expCtx)
    : AccumulatorN(expCtx) {
    _memUsageTracker.set(sizeof(*this));
}

template <FirstLastSense sense, bool single>
std::pair<long long, Value> AccumulatorFirstLastNForBucketAuto<sense, single>::_genKeyOutPair(
    const Value& val) {
    tassert(8533700,
            str::stream() << getOpName()
                          << " tried to get a sort key on something that wasn't a BSON object",
            val.isObject());

    Value output = val[kFieldNameOutput];

    // Upconvert to 'null' if the output field is missing.
    if (output.missing())
        output = Value(BSONNULL);

    Value sortKey = val[kFieldNameGeneratedSortKey];
    return {std::move(sortKey.coerceToLong()), std::move(output)};
}

template <FirstLastSense sense, bool single>
void AccumulatorFirstLastNForBucketAuto<sense, single>::_processValue(const Value& val) {
    auto keyOutPair = _genKeyOutPair(val);

    // Only insert in the lastN case if we have 'n' elements.
    if (static_cast<long long>(_map.size()) == *_n) {
        // Get an iterator to the element we want to compare against. In particular, $first will
        // insert items less than the max, and $last will insert greater than the min.
        auto [cmpElem, cmp] = [&]() {
            if constexpr (sense == Sense::kFirst) {
                auto elem = std::prev(_map.end());
                auto res = elem->first > keyOutPair.first;
                return std::make_pair(elem, res);
            } else {
                auto elem = _map.begin();
                auto res = keyOutPair.first > elem->first;
                return std::make_pair(elem, res);
            }
        }();

        // When the sort key produces a tie we keep the first value seen.
        if (cmp) {
            _map.erase(cmpElem);
        } else {
            return;
        }
    }

    const auto memUsage =
        sizeof(long long) + keyOutPair.second.getApproximateSize() + sizeof(KeyOutPair);
    _map.emplace(keyOutPair.first,
                 SimpleMemoryUsageTokenWith<Value>{
                     SimpleMemoryUsageToken{memUsage, &_memUsageTracker}, keyOutPair.second});
    checkMemUsage();
}

template <FirstLastSense sense, bool single>
const char* AccumulatorFirstLastNForBucketAuto<sense, single>::getOpName() const {
    return AccumulatorFirstLastNForBucketAuto<sense, single>::getName().rawData();
}

template <FirstLastSense sense, bool single>
Document AccumulatorFirstLastNForBucketAuto<sense, single>::serialize(
    boost::intrusive_ptr<Expression> initializer,
    boost::intrusive_ptr<Expression> argument,
    const SerializationOptions& options) const {
    MutableDocument args;
    if constexpr (single) {
        // Uses the same serialize() method as $first/$last uses, requiring a null initializer
        // instead of the one created in replaceAccumulationStatementForBucketAuto().
        auto nullInitializer =
            ExpressionConstant::create(initializer->getExpressionContext(), Value(BSONNULL));
        return AccumulatorState::serialize(std::move(nullInitializer), argument, options);
    }

    AccumulatorN::serializeHelper(initializer, argument, options, args);
    return DOC(getOpName() << args.freeze());
}

template <FirstLastSense sense, bool single>
void AccumulatorFirstLastNForBucketAuto<sense, single>::reset() {
    _map.clear();
}

template <FirstLastSense sense, bool single>
Value AccumulatorFirstLastNForBucketAuto<sense, single>::getValue(bool /*toBeMerged*/) {
    std::vector<Value> result;
    auto begin = _map.begin();
    auto end = _map.end();

    // Insert at most _n values into result.
    auto it = begin;
    for (auto inserted = 0; inserted < *_n && it != end; ++inserted, ++it) {
        const auto& keyOutPair = *it;
        result.push_back(keyOutPair.second.value());
    }

    if constexpr (!single) {
        return Value(std::move(result));
    } else {
        tassert(8533701,
                "An accumulator will always have at least one value processed in $bucketAuto",
                !result.empty());
        return Value(std::move(result[0]));
    }
}

template <FirstLastSense sense, bool single>
boost::intrusive_ptr<AccumulatorState> AccumulatorFirstLastNForBucketAuto<sense, single>::create(
    ExpressionContext* const expCtx) {
    return make_intrusive<AccumulatorFirstLastNForBucketAuto<sense, single>>(expCtx);
}

bool isPositionalAccumulator(const char* opName) {
    return factoryFnMap.find(opName) != factoryFnMap.cend();
}

AccumulationStatement replaceAccumulationStatementForBucketAuto(ExpressionContext* const expCtx,
                                                                AccumulationStatement&& stmt) {

    auto accName = stmt.expr.name;
    if (!isPositionalAccumulator(accName.rawData())) {
        return std::move(stmt);
    }
    if (!accName.endsWith("N")) {
        stmt.expr.initializer = ExpressionConstant::create(expCtx, Value(1));
    }

    auto factory = factoryFnMap[accName](expCtx);
    auto expr = AccumulationExpression{std::move(stmt.expr.initializer),
                                       std::move(stmt.expr.argument),
                                       std::move(factory),
                                       std::move(stmt.expr.name)};
    return AccumulationStatement(std::move(stmt.fieldName), std::move(expr));
}

}  // namespace mongo
