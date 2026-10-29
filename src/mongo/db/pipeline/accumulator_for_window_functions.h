// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function_covariance.h"
#include "mongo/db/pipeline/window_function/window_function_integral.h"
#include "mongo/util/modules.h"

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This class is a base class for accumulators that are applied only in window functions. Some
 * functionality supported in $group, e.g. merging on process(), is not supported in
 * AccumulatorForWindowFunctions. Some certain group-related methods should also be unreachable
 * here.
 */
class AccumulatorForWindowFunctions : public AccumulatorState {
public:
    AccumulatorForWindowFunctions(ExpressionContext* const expCtx) : AccumulatorState(expCtx) {}

    ExpressionNary::Associativity getAssociativity() const final {
        tasserted(5424002,
                  str::stream() << "Invalid call to getAssociativity() in accumulator "
                                << getOpName());
        return ExpressionNary::Associativity::kNone;
    }

    bool isCommutative() const final {
        tasserted(5424003,
                  str::stream() << "Invalid call to isCommutative in accumulator " << getOpName());
    }
};

class AccumulatorCovariance : public AccumulatorForWindowFunctions {
public:
    AccumulatorCovariance(ExpressionContext* expCtx, bool isSamp);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    void reset() final;

    const char* getOpName() const final {
        return (_covarianceWF.isSample() ? "$covarianceSamp" : "$covariancePop");
    }

private:
    WindowFunctionCovariance _covarianceWF;
};

class AccumulatorCovarianceSamp final : public AccumulatorCovariance {
public:
    static constexpr auto kName = "$covarianceSamp"sv;

    explicit AccumulatorCovarianceSamp(ExpressionContext* const expCtx)
        : AccumulatorCovariance(expCtx, true) {}
};

class AccumulatorCovariancePop final : public AccumulatorCovariance {
public:
    static constexpr auto kName = "$covariancePop"sv;

    explicit AccumulatorCovariancePop(ExpressionContext* const expCtx)
        : AccumulatorCovariance(expCtx, false) {}
};

class AccumulatorRankBase : public AccumulatorForWindowFunctions {
public:
    // The modern constrcutor. When constructed this way, we'll interpret the input values as
    // directly comparable sort keys, which is simpler.
    explicit AccumulatorRankBase(ExpressionContext* expCtx);
    // Legacy constructor. If constructed this way, we'll interpret the input values as raw inputs
    // which need to be compared carefully (using the comparator to respect collations, and
    // traversing arrays, etc.).
    explicit AccumulatorRankBase(ExpressionContext* expCtx, bool isAscending);
    void reset() override;

    Value getValue(bool toBeMerged) final {
        return Value::createIntOrLong(_lastRank);
    }

protected:
    bool isNewValue(Value thisInput);

    long long _lastRank = 0;
    boost::optional<Value> _lastInput = boost::none;
    boost::optional<SortKeyGenerator> _legacySortKeyGen;
};

class AccumulatorRank : public AccumulatorRankBase {
public:
    static constexpr auto kName = "$rank"sv;

    const char* getOpName() const final {
        return kName.data();
    }
    // Modern constructor.
    explicit AccumulatorRank(ExpressionContext* const expCtx) : AccumulatorRankBase(expCtx) {}

    // Legacy constructor
    explicit AccumulatorRank(ExpressionContext* const expCtx, bool isAscending)
        : AccumulatorRankBase(expCtx, isAscending) {}

    void processInternal(const Value& input, bool merging) final;
    void reset() final;

private:
    size_t _numSameRank = 1;
};

class AccumulatorDocumentNumber : public AccumulatorRankBase {
public:
    static constexpr auto kName = "$documentNumber"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    // Modern constructor.
    explicit AccumulatorDocumentNumber(ExpressionContext* const expCtx)
        : AccumulatorRankBase(expCtx) {}

    // Legacy constructor
    explicit AccumulatorDocumentNumber(ExpressionContext* const expCtx, bool isAscending)
        : AccumulatorRankBase(expCtx, isAscending) {}
    void processInternal(const Value& input, bool merging) final;
};

class AccumulatorDenseRank : public AccumulatorRankBase {
public:
    static constexpr auto kName = "$denseRank"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    // Modern constructor.
    explicit AccumulatorDenseRank(ExpressionContext* const expCtx) : AccumulatorRankBase(expCtx) {}

    // Legacy constructor
    explicit AccumulatorDenseRank(ExpressionContext* const expCtx, bool isAscending)
        : AccumulatorRankBase(expCtx, isAscending) {}

    void processInternal(const Value& input, bool merging) final;
};

class AccumulatorIntegral : public AccumulatorForWindowFunctions {
public:
    static constexpr auto kName = "$integral"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    explicit AccumulatorIntegral(ExpressionContext* expCtx,
                                 boost::optional<long long> unitMillis = boost::none);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    void reset() final;

private:
    WindowFunctionIntegral _integralWF;
};

class AccumulatorLocf : public AccumulatorForWindowFunctions {
public:
    static constexpr auto kName = "$locf"sv;

    const char* getOpName() const final {
        return kName.data();
    }

    explicit AccumulatorLocf(ExpressionContext* expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    void reset() final;

private:
    Value _lastNonNull{BSONNULL};
};

}  // namespace mongo
