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

#include <deque>

#include "mongo/db/pipeline/accumulation_statement.h"

namespace mongo {

/**
 * An AccumulatorN picks 'n' of its input values and returns them in an array. Each derived class
 * has different criteria for how to pick values and order the final array, but any common behavior
 * shared by derived classes is implemented in this class. In particular:
 * - Initializing 'n' during 'startNewGroup'.
 * - Parsing the expressions for 'n' and 'output'.
 */
class AccumulatorN : public AccumulatorState {
public:
    AccumulatorN(ExpressionContext* expCtx);

    void processInternal(const Value& input, bool merging) final;

    // Initialize 'n' with 'input'. In particular, verifies that 'input' is a positive integer.
    void startNewGroup(const Value& input) final;

    /**
     * Helper which appends the 'n' and 'output' fields to 'md'.
     */
    static void serializeHelper(const boost::intrusive_ptr<Expression>& initializer,
                                const boost::intrusive_ptr<Expression>& argument,
                                bool explain,
                                MutableDocument& md);

protected:
    // Parses 'args' for the 'n' and 'output' arguments that are common to the 'N' family of
    // accumulators.
    static std::tuple<boost::intrusive_ptr<Expression>, boost::intrusive_ptr<Expression>> parseArgs(
        ExpressionContext* expCtx, const BSONObj& args, VariablesParseState vps);

    // Stores the limit of how many values we will return. This value is initialized to
    // 'boost::none' on construction and is only set during 'startNewGroup'.
    boost::optional<long long> _n;

    int _maxMemUsageBytes = 0;

private:
    static constexpr auto kFieldNameN = "n"_sd;
    static constexpr auto kFieldNameOutput = "output"_sd;

    virtual void processValue(const Value& val) = 0;
};
class AccumulatorMinMaxN : public AccumulatorN {
public:
    using MinMaxSense = AccumulatorMinMax::Sense;

    AccumulatorMinMaxN(ExpressionContext* expCtx, MinMaxSense sense);

    /**
     * Verifies that 'elem' is an object, delegates argument parsing to 'AccumulatorN::parseArgs',
     * and constructs an AccumulationExpression representing $minN or $maxN depending on 's'.
     */
    template <MinMaxSense s>
    static AccumulationExpression parseMinMaxN(ExpressionContext* expCtx,
                                               BSONElement elem,
                                               VariablesParseState vps);

    /**
     * Constructs an Expression representing $minN or $maxN depending on 's'.
     */
    template <MinMaxSense s>
    static boost::intrusive_ptr<Expression> parseExpression(ExpressionContext* expCtx,
                                                            BSONElement exprElement,
                                                            const VariablesParseState& vps);

    Value getValue(bool toBeMerged) final;

    const char* getOpName() const final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       bool explain) const final;

    void reset() final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

private:
    void processValue(const Value& val) final;

    ValueMultiset _set;
    MinMaxSense _sense;
};

class AccumulatorMinN : public AccumulatorMinMaxN {
public:
    static constexpr auto kName = "$minN"_sd;
    explicit AccumulatorMinN(ExpressionContext* expCtx)
        : AccumulatorMinMaxN(expCtx, MinMaxSense::kMin) {}

    static const char* getName();

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorMaxN : public AccumulatorMinMaxN {
public:
    static constexpr auto kName = "$maxN"_sd;
    explicit AccumulatorMaxN(ExpressionContext* expCtx)
        : AccumulatorMinMaxN(expCtx, MinMaxSense::kMax) {}

    static const char* getName();

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorFirstLastN : public AccumulatorN {
public:
    enum Sense : int {
        kFirst = 1,
        kLast = -1,
    };

    AccumulatorFirstLastN(ExpressionContext* expCtx, Sense variant);

    /**
     * Verifies that 'elem' is an object, delegates argument parsing to 'AccumulatorN::parseArgs',
     * and constructs an AccumulationExpression representing $firstN or $lastN depending on 's'.
     */
    template <Sense s>
    static AccumulationExpression parseFirstLastN(ExpressionContext* expCtx,
                                                  BSONElement elem,
                                                  VariablesParseState vps);

    const char* getOpName() const final;

    Document serialize(boost::intrusive_ptr<Expression> initializer,
                       boost::intrusive_ptr<Expression> argument,
                       bool explain) const final;

    void reset() final;

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return true;
    }

    Value getValue(bool toBeMerged) final;

private:
    // firstN/lastN do NOT ignore null values.
    void processValue(const Value& val) final;

    std::deque<Value> _deque;
    Sense _variant;
};

class AccumulatorFirstN : public AccumulatorFirstLastN {
public:
    static constexpr auto kName = "$firstN"_sd;
    explicit AccumulatorFirstN(ExpressionContext* expCtx)
        : AccumulatorFirstLastN(expCtx, Sense::kFirst) {}

    static const char* getName();

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};

class AccumulatorLastN : public AccumulatorFirstLastN {
public:
    static constexpr auto kName = "$lastN"_sd;
    explicit AccumulatorLastN(ExpressionContext* expCtx)
        : AccumulatorFirstLastN(expCtx, Sense::kLast) {}

    static const char* getName();

    static boost::intrusive_ptr<AccumulatorState> create(ExpressionContext* expCtx);
};
}  // namespace mongo
