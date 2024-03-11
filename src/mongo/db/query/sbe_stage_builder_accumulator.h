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

#include <boost/optional/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/util/string_map.h"

namespace mongo::stage_builder {
class PlanStageSlots;

namespace Accum {
struct OpInfo;

struct BlockAggAndRowAgg {
    SbExpr blockAgg;
    SbExpr rowAgg;
};

// This class serves as the base class for all the "Inputs" classes used by the build methods.
struct Inputs {
    Inputs() = default;

    virtual ~Inputs();
};

using InputsPtr = std::unique_ptr<Inputs>;

struct AccumBlockExprs {
    InputsPtr inputs;
    SbExpr::Vector exprs;
    SbSlotVector slots;
};

class Op {
public:
    Op(std::string opName);

    Op(StringData opName) : Op(opName.toString()) {}

    Op(const AccumulationStatement& acc);

    const std::string& getOpName() const {
        return _opName;
    }

    bool countAddendIsIntegerOrDouble() const {
        return _countAddendIsIntegerOrDouble;
    }

    /**
     * This method returns the number of agg expressions that need to be generated for this
     * Op.
     *
     * $avg generates 2 agg expressions, while most other ops only generate 1 agg expression.
     */
    size_t getNumAggs() const;

    /**
     * This method returns true if this Op supports buildAccumBlockExprs(), otherwise
     * returns false.
     *
     * When hasBuildAccumBlockExprs() is false, calling buildAccumBlockExprs() will always
     * return boost::none.
     */
    bool hasBuildAccumBlockExprs() const;

    /**
     * This method returns true if this Op supports buildAccumBlockAggs(), otherwise
     * returns false.
     *
     * When hasBuildAccumBlockAggs() is false, calling buildAccumBlockAggs() will always
     * return boost::none.
     */
    bool hasBuildAccumBlockAggs() const;

    /**
     * Given one or more input expressions ('input' / 'inputs'), these methods generate the
     * arg expressions needed for this Op.
     */
    InputsPtr buildAccumExprs(StageBuilderState& state, InputsPtr inputs) const;

    /**
     * Given one or more input expressions ('input' / 'inputs'), these methods generate the
     * "block" versions of the arg expressions needed for this Op.
     */
    boost::optional<AccumBlockExprs> buildAccumBlockExprs(StageBuilderState& state,
                                                          InputsPtr inputs,
                                                          const PlanStageSlots& outputs) const;

    /**
     * Given a vector of named arg expressions ('args' / 'argNames'), this method generates the
     * accumulate expressions for this Op.
     */
    SbExpr::Vector buildAccumAggs(StageBuilderState& state, InputsPtr inputs) const;

    /**
     * Given a vector of the "block" versions of the arg expressions ('args' / 'argNames'), this
     * method generates the "block" versions of the accumulate expressions for this Op.
     */
    boost::optional<std::vector<BlockAggAndRowAgg>> buildAccumBlockAggs(
        StageBuilderState& state,
        InputsPtr inputs,
        SbSlot bitmapInternalSlot,
        SbSlot accInternalSlot) const;

    /**
     * Given a map of input expressions ('argExprs'), these methods generate the initialize
     * expressions for this Op.
     */
    SbExpr::Vector buildInitialize(StageBuilderState& state, InputsPtr inputs) const;

    /**
     * Given a map of input expressions ('argExprs'), this method generates the finalize
     * expression for this Op.
     */
    SbExpr buildFinalize(StageBuilderState& state,
                         InputsPtr inputs,
                         const SbSlotVector& aggSlots) const;

    /**
     * When SBE hash aggregation spills to disk, it spills partial aggregates which need to be
     * combined later. This method returns the expressions that can be used to combine partial
     * aggregates for this Op. The aggregate-of-aggregates will be stored in a slots
     * owned by the hash agg stage, while the new partial aggregates to combine can be read from
     * the given 'inputSlots'.
     */
    SbExpr::Vector buildCombineAggs(StageBuilderState& state,
                                    InputsPtr inputs,
                                    const SbSlotVector& inputSlots) const;

private:
    // Static helper method for looking up the info for this Op in the global map.
    // This method should only be used by Op's constructors.
    static const OpInfo* lookupOpInfo(const std::string& opName);

    // Non-static checked helper method for retrieving the value of '_opInfo'. This method will
    // raise a tassert if '_opInfo' is null.
    const OpInfo* getOpInfo() const {
        uassert(8751302,
                str::stream() << "Unrecognized AccumulatorOp name: " << _opName,
                _opInfo != nullptr);

        return _opInfo;
    }

    // Name of the specific accumulation op. This name is used to retrieve info about the op
    // from the global map.
    std::string _opName;

    // Info about the specific accumulation op named by '_opName'.
    const OpInfo* _opInfo = nullptr;

    // Flag that indicates if this is a "$sum" op whose input is an integer constant or a
    // double constant.
    bool _countAddendIsIntegerOrDouble = false;
};

extern const StringData kCount;
extern const StringData kCovarianceX;
extern const StringData kCovarianceY;
extern const StringData kDefaultVal;
extern const StringData kInput;
extern const StringData kInputFirst;
extern const StringData kInputLast;
extern const StringData kIsAscending;
extern const StringData kIsGroupAccum;
extern const StringData kMaxSize;
extern const StringData kSortBy;
extern const StringData kSortByFirst;
extern const StringData kSortByLast;
extern const StringData kSortSpec;
extern const StringData kUnit;
extern const StringData kValue;

extern const std::vector<std::string> kAccumulatorSingleParam;
extern const std::vector<std::string> kAccumulatorAvgParams;
extern const std::vector<std::string> kAccumulatorCovarianceParams;
extern const std::vector<std::string> kAccumulatorDenseRankParams;
extern const std::vector<std::string> kAccumulatorIntegralParams;
extern const std::vector<std::string> kAccumulatorLinearFillParams;
extern const std::vector<std::string> kAccumulatorRankParams;
extern const std::vector<std::string> kAccumulatorTopBottomNParams;

struct AccumSingleInput : public Inputs {
    AccumSingleInput(SbExpr inputExpr) : inputExpr(std::move(inputExpr)) {}

    AccumSingleInput(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
};

struct AccumAggsAvgInputs : public Inputs {
    AccumAggsAvgInputs(SbExpr inputExpr, SbExpr count)
        : inputExpr(std::move(inputExpr)), count(std::move(count)) {}

    AccumAggsAvgInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
    SbExpr count;
};

struct AccumCovarianceInputs : public Inputs {
    AccumCovarianceInputs(SbExpr covarianceX, SbExpr covarianceY)
        : covarianceX(std::move(covarianceX)), covarianceY(std::move(covarianceY)) {}

    AccumCovarianceInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr covarianceX;
    SbExpr covarianceY;
};

struct AccumRankInputs : public Inputs {
    AccumRankInputs(SbExpr inputExpr, SbExpr isAscending)
        : inputExpr(std::move(inputExpr)), isAscending(std::move(isAscending)) {}

    AccumRankInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
    SbExpr isAscending;
};

struct AccumIntegralInputs : public Inputs {
    AccumIntegralInputs(SbExpr inputExpr, SbExpr sortBy)
        : inputExpr(std::move(inputExpr)), sortBy(std::move(sortBy)) {}

    AccumIntegralInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
    SbExpr sortBy;
};

struct AccumLinearFillInputs : public Inputs {
    AccumLinearFillInputs(SbExpr inputExpr, SbExpr sortBy)
        : inputExpr(std::move(inputExpr)), sortBy(std::move(sortBy)) {}

    AccumLinearFillInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
    SbExpr sortBy;
};

struct AccumTopBottomNInputs : public Inputs {
    AccumTopBottomNInputs(SbExpr value, SbExpr sortBy, SbExpr sortSpec)
        : value(std::move(value)), sortBy(std::move(sortBy)), sortSpec(std::move(sortSpec)) {}

    AccumTopBottomNInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr value;
    SbExpr sortBy;
    SbExpr sortSpec;
};

struct InitAccumNInputs : public Inputs {
    InitAccumNInputs(SbExpr maxSize, SbExpr isGroupAccum)
        : maxSize(std::move(maxSize)), isGroupAccum(std::move(isGroupAccum)) {}

    InitAccumNInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr maxSize;
    SbExpr isGroupAccum;
};

struct InitExpMovingAvgInputs : public Inputs {
    InitExpMovingAvgInputs(SbExpr inputExpr) : inputExpr(std::move(inputExpr)) {}

    InitExpMovingAvgInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
};

struct InitIntegralInputs : public Inputs {
    InitIntegralInputs(SbExpr inputExpr) : inputExpr(std::move(inputExpr)) {}

    InitIntegralInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr inputExpr;
};

struct FinalizeTopBottomNInputs : public Inputs {
    FinalizeTopBottomNInputs(SbExpr sortSpec) : sortSpec(std::move(sortSpec)) {}

    FinalizeTopBottomNInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr sortSpec;
};

struct FinalizeDerivativeInputs : public Inputs {
    FinalizeDerivativeInputs(
        SbExpr unit, SbExpr inputFirst, SbExpr sortByFirst, SbExpr inputLast, SbExpr sortByLast)
        : unit(std::move(unit)),
          inputFirst(std::move(inputFirst)),
          sortByFirst(std::move(sortByFirst)),
          inputLast(std::move(inputLast)),
          sortByLast(std::move(sortByLast)) {}

    FinalizeDerivativeInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr unit;
    SbExpr inputFirst;
    SbExpr sortByFirst;
    SbExpr inputLast;
    SbExpr sortByLast;
};

struct FinalizeLinearFillInputs : public Inputs {
    FinalizeLinearFillInputs(SbExpr sortBy) : sortBy(std::move(sortBy)) {}

    FinalizeLinearFillInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr sortBy;
};

struct CombineAggsTopBottomNInputs : public Inputs {
    CombineAggsTopBottomNInputs(SbExpr sortSpec) : sortSpec(std::move(sortSpec)) {}

    CombineAggsTopBottomNInputs(StringDataMap<std::unique_ptr<sbe::EExpression>> args);

    SbExpr sortSpec;
};

// This wrapper class exists so that the SBE window function implementation can pass in named
// expr maps to Accum::Op's build methods. Once the SBE window function implementation has been
// converted to use subclasses of Accum::Inputs, we can delete this wrapper (and any assoicated
// logic needed to make it work).
struct NamedExprsMapWrapper : public Inputs {
    NamedExprsMapWrapper(StringDataMap<std::unique_ptr<sbe::EExpression>> args)
        : args(std::move(args)) {}

    StringDataMap<std::unique_ptr<sbe::EExpression>> args;
};
}  // namespace Accum
}  // namespace mongo::stage_builder
