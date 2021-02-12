/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/summation.h"

namespace mongo {

/**
 * This enum indicates which documents an accumulator needs to see in order to compute its output.
 */
enum class AccumulatorDocumentsNeeded {
    // AccumulatorState needs to see all documents in a group.
    kAllDocuments,

    // AccumulatorState only needs to see one document in a group, and when there is a sort order,
    // that document must be the first document.
    kFirstDocument,

    // AccumulatorState only needs to see one document in a group, and when there is a sort order,
    // that document must be the last document.
    kLastDocument,
};

class AccumulatorState : public RefCountable {
public:
    using Factory = std::function<boost::intrusive_ptr<AccumulatorState>()>;

    AccumulatorState(ExpressionContext* const expCtx) : _expCtx(expCtx) {}

    /** Marks the beginning of a new group. The input is the result of evaluating
     *  AccumulatorExpression::initializer, which can read from the group key.
     */
    virtual void startNewGroup(const Value& input) {}

    /** Process input and update internal state.
     *  merging should be true when processing outputs from getValue(true).
     */
    void process(const Value& input, bool merging) {
        processInternal(input, merging);
    }

    /**
     * Finish processing all the pending operations, and clean up memory. Some accumulators
     * ($accumulator for example) might do a batch processing in order to improve performace. In
     * those cases, the memory consumption could spike up. Calling this function can help flush
     * those batch.
     */
    virtual void reduceMemoryConsumptionIfAble() {}

    /** Marks the end of the evaluate() phase and return accumulated result.
     *  toBeMerged should be true when the outputs will be merged by process().
     */
    virtual Value getValue(bool toBeMerged) = 0;

    /// The name of the op as used in a serialization of the pipeline.
    virtual const char* getOpName() const = 0;

    int memUsageForSorter() const {
        dassert(_memUsageBytes != 0);  // This would mean subclass didn't set it
        return _memUsageBytes;
    }

    /// Reset this accumulator to a fresh state, ready for a new call to startNewGroup.
    virtual void reset() = 0;

    virtual bool isAssociative() const {
        return false;
    }

    virtual bool isCommutative() const {
        return false;
    }

    /**
     * Serializes this accumulator to a valid MQL accumulation statement that would be legal
     * inside a $group.
     *
     * The parameter expression represents the input to any accumulator created by the
     * serialized accumulation statement.
     *
     * When executing on a sharded cluster, the result of this function will be sent to each
     * individual shard.
     *
     * This implementation assumes the accumulator has the simple syntax { <name>: <argument> },
     * such as { $sum: <argument> }. This syntax has no room for an initializer. Subclasses with a
     * more elaborate syntax such should override this method.
     */
    virtual Document serialize(boost::intrusive_ptr<Expression> initializer,
                               boost::intrusive_ptr<Expression> argument,
                               bool explain) const {
        ExpressionConstant const* ec = dynamic_cast<ExpressionConstant const*>(initializer.get());
        invariant(ec);
        invariant(ec->getValue().nullish());

        return DOC(getOpName() << argument->serialize(explain));
    }

    virtual AccumulatorDocumentsNeeded documentsNeeded() const {
        return AccumulatorDocumentsNeeded::kAllDocuments;
    }

protected:
    /// Update subclass's internal state based on input
    virtual void processInternal(const Value& input, bool merging) = 0;

    auto getExpressionContext() const {
        return _expCtx;
    }

    /// subclasses are expected to update this as necessary
    int _memUsageBytes = 0;

private:
    ExpressionContext* _expCtx;
};

class AccumulatorAddToSet final : public AccumulatorState {
public:
    /**
     * Creates a new $addToSet accumulator. If no memory limit is given, defaults to the value of
     * the server parameter 'internalQueryMaxAddToSetBytes'.
     */
    AccumulatorAddToSet(ExpressionContext* const expCtx,
                        boost::optional<int> maxMemoryUsageBytes = boost::none);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    ValueUnorderedSet _set;
    int _maxMemUsageBytes;
};

class AccumulatorFirst final : public AccumulatorState {
public:
    explicit AccumulatorFirst(ExpressionContext* const expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

    AccumulatorDocumentsNeeded documentsNeeded() const final {
        return AccumulatorDocumentsNeeded::kFirstDocument;
    }

private:
    bool _haveFirst;
    Value _first;
};

class AccumulatorLast final : public AccumulatorState {
public:
    explicit AccumulatorLast(ExpressionContext* const expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

    AccumulatorDocumentsNeeded documentsNeeded() const final {
        return AccumulatorDocumentsNeeded::kLastDocument;
    }

private:
    Value _last;
};

class AccumulatorSum final : public AccumulatorState {
public:
    explicit AccumulatorSum(ExpressionContext* const expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    BSONType totalType = NumberInt;
    DoubleDoubleSummation nonDecimalTotal;
    Decimal128 decimalTotal;
};

class AccumulatorMinMax : public AccumulatorState {
public:
    enum Sense : int {
        MIN = 1,
        MAX = -1,  // Used to "scale" comparison.
    };

    AccumulatorMinMax(ExpressionContext* const expCtx, Sense sense);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    Value _val;
    const Sense _sense;
};

class AccumulatorMax final : public AccumulatorMinMax {
public:
    explicit AccumulatorMax(ExpressionContext* const expCtx) : AccumulatorMinMax(expCtx, MAX) {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);
};

class AccumulatorMin final : public AccumulatorMinMax {
public:
    explicit AccumulatorMin(ExpressionContext* const expCtx) : AccumulatorMinMax(expCtx, MIN) {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);
};

class AccumulatorPush final : public AccumulatorState {
public:
    /**
     * Creates a new $push accumulator. If no memory limit is given, defaults to the value of the
     * server parameter 'internalQueryMaxPushBytes'.
     */
    AccumulatorPush(ExpressionContext* const expCtx,
                    boost::optional<int> maxMemoryUsageBytes = boost::none);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

private:
    std::vector<Value> _array;
    int _maxMemUsageBytes;
};

class AccumulatorAvg final : public AccumulatorState {
public:
    explicit AccumulatorAvg(ExpressionContext* const expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

private:
    /**
     * The total of all values is partitioned between those that are decimals, and those that are
     * not decimals, so the decimal total needs to add the non-decimal.
     */
    Decimal128 _getDecimalTotal() const;

    bool _isDecimal;
    DoubleDoubleSummation _nonDecimalTotal;
    Decimal128 _decimalTotal;
    long long _count;
};

class AccumulatorStdDev : public AccumulatorState {
public:
    AccumulatorStdDev(ExpressionContext* const expCtx, bool isSamp);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

private:
    const bool _isSamp;
    long long _count;
    double _mean;
    double _m2;  // Running sum of squares of delta from mean. Named to match algorithm.
};

class AccumulatorStdDevPop final : public AccumulatorStdDev {
public:
    explicit AccumulatorStdDevPop(ExpressionContext* const expCtx)
        : AccumulatorStdDev(expCtx, false) {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);
};

class AccumulatorStdDevSamp final : public AccumulatorStdDev {
public:
    explicit AccumulatorStdDevSamp(ExpressionContext* const expCtx)
        : AccumulatorStdDev(expCtx, true) {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);
};

class AccumulatorMergeObjects : public AccumulatorState {
public:
    AccumulatorMergeObjects(ExpressionContext* const expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    const char* getOpName() const final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* const expCtx);

private:
    MutableDocument _output;
};

}  // namespace mongo
