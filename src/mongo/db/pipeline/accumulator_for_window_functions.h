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

#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/window_function/window_function_covariance.h"
#include "mongo/db/pipeline/window_function/window_function_integral.h"

namespace mongo {

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
    static constexpr auto kName = "$covarianceSamp"_sd;

    explicit AccumulatorCovarianceSamp(ExpressionContext* const expCtx)
        : AccumulatorCovariance(expCtx, true) {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorCovariancePop final : public AccumulatorCovariance {
public:
    static constexpr auto kName = "$covariancePop"_sd;

    explicit AccumulatorCovariancePop(ExpressionContext* const expCtx)
        : AccumulatorCovariance(expCtx, false) {}
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorRankBase : public AccumulatorForWindowFunctions {
public:
    explicit AccumulatorRankBase(ExpressionContext* expCtx);
    void reset();

    Value getValue(bool toBeMerged) final {
        return Value::createIntOrLong(_lastRank);
    }

protected:
    long long _lastRank = 0;
    boost::optional<Value> _lastInput = boost::none;
};

class AccumulatorRank : public AccumulatorRankBase {
public:
    static constexpr auto kName = "$rank"_sd;

    const char* getOpName() const final {
        return kName.rawData();
    }

    explicit AccumulatorRank(ExpressionContext* const expCtx) : AccumulatorRankBase(expCtx) {}
    void processInternal(const Value& input, bool merging) final;
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
    void reset() final;

private:
    size_t _numSameRank = 1;
};

class AccumulatorDocumentNumber : public AccumulatorRankBase {
public:
    static constexpr auto kName = "$documentNumber"_sd;

    const char* getOpName() const final {
        return kName.rawData();
    }

    explicit AccumulatorDocumentNumber(ExpressionContext* const expCtx)
        : AccumulatorRankBase(expCtx) {}
    void processInternal(const Value& input, bool merging) final;
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorDenseRank : public AccumulatorRankBase {
public:
    static constexpr auto kName = "$denseRank"_sd;

    const char* getOpName() const final {
        return kName.rawData();
    }

    explicit AccumulatorDenseRank(ExpressionContext* const expCtx) : AccumulatorRankBase(expCtx) {}
    void processInternal(const Value& input, bool merging) final;
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorIntegral : public AccumulatorForWindowFunctions {
public:
    static constexpr auto kName = "$integral"_sd;

    const char* getOpName() const final {
        return kName.rawData();
    }

    explicit AccumulatorIntegral(ExpressionContext* expCtx,
                                 boost::optional<long long> unitMillis = boost::none);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    void reset() final;

    static boost::intrusive_ptr<AccumulatorState> create(
        ExpressionContext* expCtx, boost::optional<long long> unitMillis = boost::none);

private:
    WindowFunctionIntegral _integralWF;
};

class AccumulatorLocf : public AccumulatorForWindowFunctions {
public:
    static constexpr auto kName = "$locf"_sd;

    const char* getOpName() const final {
        return kName.rawData();
    }

    explicit AccumulatorLocf(ExpressionContext* expCtx);

    void processInternal(const Value& input, bool merging) final;
    Value getValue(bool toBeMerged) final;
    void reset() final;
    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);

private:
    Value _lastNonNull{BSONNULL};
};

}  // namespace mongo
