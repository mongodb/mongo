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
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
const StringData kAccumulatorCountName = "$count"_sd;

class PlanStageSlots;

// This class serves as the base class for all the "AccumInputs" classes used by the build methods.
struct AccumInputs {
    AccumInputs() = default;

    virtual ~AccumInputs();

    virtual std::unique_ptr<AccumInputs> clone() const = 0;
};

using AccumInputsPtr = std::unique_ptr<AccumInputs>;

struct BlockAggAndRowAgg {
    SbExpr blockAgg;
    SbExpr rowAgg;
};

struct AddBlockExprs {
    AccumInputsPtr inputs;
    SbExpr::Vector exprs;
    SbSlotVector slots;
};

struct AccumOpInfo;

class AccumOp {
public:
    AccumOp(std::string opName);

    AccumOp(StringData opName) : AccumOp(std::string{opName}) {}

    AccumOp(const AccumulationStatement& acc);

    StringData getOpName() const {
        return _opName;
    }

    /**
     * Returns the name of this accumulator, such as "$avg" or "$count". AccumulationStatement does
     * not save this, and AccumulationStatement.AccumulationExpression.name is not always the
     * correct name (e.g. it shows "$sum" for a $count accumulator).
     */
    static std::string getOpNameForAccStmt(const AccumulationStatement& accStmt);

    /**
     * This method returns the number of agg expressions that need to be generated for this
     * AccumOp.
     *
     * $avg generates 2 agg expressions, while most other ops only generate 1 agg expression.
     */
    size_t getNumAggs() const;

    /**
     * This method returns true if this AccumOp supports buildAddBlockExprs(), otherwise
     * returns false.
     *
     * When hasBuildAddBlockExprs() is false, calling buildAddBlockExprs() will always
     * return boost::none.
     */
    bool hasBuildAddBlockExprs() const;

    /**
     * This method returns true if this AccumOp supports buildAddBlockAggs(), otherwise
     * returns false.
     *
     * When hasBuildAddBlockAggs() is false, calling buildAddBlockAggs() will always
     * return boost::none.
     */
    bool hasBuildAddBlockAggs() const;

    /**
     * This method returns true this AccumOp supports buildSinglePurposeAccumulator(), otherwise
     * returns false.
     */
    bool canBuildSinglePurposeAccumulator() const;

    /**
     * This method generates an expression for a single-purpose accumulator that has native
     * implementations of accumulator operations instead of "add," "initialize," and "combine" ABT
     * expressions. Used when the desired output is the final result of accumulation.
     *
     * Only call this method after ensuring that canBuildSinglePrposeAccumulator() returns true.
     */
    SbHashAggAccumulator buildSinglePurposeAccumulator(StageBuilderState& state,
                                                       SbExpr inputExpression,
                                                       std::string fieldName,
                                                       SbSlot outSlot,
                                                       SbSlot spillSlot) const;

    /**
     * Same as buildSinglePurposeAccumulator() but used when the desired result is a partial
     * aggregate, such as should be produced by shard pipelines whose results are to be merged.
     *
     * Only call this method after ensuring that canBuildSinglePrposeAccumulator() returns true.
     */
    SbHashAggAccumulator buildSinglePurposeAccumulatorForMerge(StageBuilderState& state,
                                                               SbExpr inputExpression,
                                                               std::string fieldName,
                                                               SbSlot outSlot,
                                                               SbSlot spillSlot) const;

    /**
     * Given one or more input expressions ('input' / 'inputs'), these methods generate the
     * arg expressions needed for this AccumOp.
     */
    AccumInputsPtr buildAddExprs(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given one or more input expressions ('input' / 'inputs'), these methods generate the
     * "block" versions of the arg expressions needed for this AccumOp.
     */
    boost::optional<AddBlockExprs> buildAddBlockExprs(StageBuilderState& state,
                                                      AccumInputsPtr inputs,
                                                      const PlanStageSlots& outputs) const;

    /**
     * Given a vector of named arg expressions ('args' / 'argNames'), this method generates the
     * accumulate expressions for this AccumOp.
     */
    SbExpr::Vector buildAddAggs(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given a vector of the "block" versions of the arg expressions ('args' / 'argNames'), this
     * method generates the "block" versions of the accumulate expressions for this AccumOp.
     */
    boost::optional<std::vector<BlockAggAndRowAgg>> buildAddBlockAggs(
        StageBuilderState& state, AccumInputsPtr inputs, SbSlot bitmapInternalSlot) const;

    /**
     * Given a map of input expressions ('argExprs'), these methods generate the initialize
     * expressions for this AccumOp.
     */
    SbExpr::Vector buildInitialize(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given a map of input expressions ('argExprs'), this method generates the finalize
     * expression for this AccumOp.
     */
    SbExpr buildFinalize(StageBuilderState& state,
                         AccumInputsPtr inputs,
                         const SbSlotVector& aggSlots) const;

    /**
     * When SBE hash aggregation spills to disk, it spills partial aggregates which need to be
     * combined later. This method returns the expressions that can be used to combine partial
     * aggregates for this AccumOp. The aggregate-of-aggregates will be stored in a slots
     * owned by the hash agg stage, while the new partial aggregates to combine can be read from
     * the given 'inputSlots'.
     */
    SbExpr::Vector buildCombineAggs(StageBuilderState& state,
                                    AccumInputsPtr inputs,
                                    const SbSlotVector& inputSlots) const;

private:
    // Static helper method for looking up the info for this AccumOp in the global map.
    // This method should only be used by AccumOp's constructors.
    static const AccumOpInfo* lookupOpInfo(const std::string& opName);

    // Non-static checked helper method for retrieving the value of '_opInfo'. This method will
    // raise a tassert if '_opInfo' is null.
    const AccumOpInfo* getOpInfo() const {
        uassert(8751302,
                str::stream() << "Unrecognized AccumulatorOp name: " << _opName,
                _opInfo != nullptr);

        return _opInfo;
    }

    // Name of the specific accumulation op. This name is used to retrieve info about the op
    // from the global map.
    std::string _opName;

    // Info about the specific accumulation op named by '_opName'.
    const AccumOpInfo* _opInfo = nullptr;
};  // class AccumOp

struct AddSingleInput : public AccumInputs {
    AddSingleInput(SbExpr inputExpr) : inputExpr(std::move(inputExpr)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
};

struct AddAggsAvgInputs : public AccumInputs {
    AddAggsAvgInputs(SbExpr inputExpr, SbExpr count)
        : inputExpr(std::move(inputExpr)), count(std::move(count)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
    SbExpr count;
};

struct AddCovarianceInputs : public AccumInputs {
    AddCovarianceInputs(SbExpr covarianceX, SbExpr covarianceY)
        : covarianceX(std::move(covarianceX)), covarianceY(std::move(covarianceY)) {}

    AccumInputsPtr clone() const final;

    SbExpr covarianceX;
    SbExpr covarianceY;
};

struct AddRankInputs : public AccumInputs {
    AddRankInputs(SbExpr inputExpr, SbExpr isAscending)
        : inputExpr(std::move(inputExpr)), isAscending(std::move(isAscending)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
    SbExpr isAscending;
};

struct AddIntegralInputs : public AccumInputs {
    AddIntegralInputs(SbExpr inputExpr, SbExpr sortBy)
        : inputExpr(std::move(inputExpr)), sortBy(std::move(sortBy)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
    SbExpr sortBy;
};

struct AddLinearFillInputs : public AccumInputs {
    AddLinearFillInputs(SbExpr inputExpr, SbExpr sortBy)
        : inputExpr(std::move(inputExpr)), sortBy(std::move(sortBy)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
    SbExpr sortBy;
};

struct AddTopBottomNInputs : public AccumInputs {
    AddTopBottomNInputs(SbExpr value, SbExpr sortBy, SbExpr sortSpec)
        : value(std::move(value)), sortBy(std::move(sortBy)), sortSpec(std::move(sortSpec)) {}

    AccumInputsPtr clone() const final;

    SbExpr value;
    SbExpr sortBy;
    SbExpr sortSpec;
};

struct AddBlockTopBottomNInputs : public AccumInputs {
    AddBlockTopBottomNInputs(std::pair<SbExpr::Vector, bool> value,
                             std::pair<SbExpr::Vector, bool> sortBy,
                             SbExpr sortSpec)
        : values(std::move(value.first)),
          sortBy(std::move(sortBy.first)),
          sortSpec(std::move(sortSpec)),
          valueIsArray(value.second),
          useMK(sortBy.second) {}

    AccumInputsPtr clone() const final;

    SbExpr::Vector values;
    SbExpr::Vector sortBy;
    SbExpr sortSpec;
    bool valueIsArray = false;
    bool useMK = false;
};

struct InitAccumNInputs : public AccumInputs {
    InitAccumNInputs(SbExpr maxSize, SbExpr isGroupAccum)
        : maxSize(std::move(maxSize)), isGroupAccum(std::move(isGroupAccum)) {}

    AccumInputsPtr clone() const final;

    SbExpr maxSize;
    SbExpr isGroupAccum;
};

struct InitExpMovingAvgInputs : public AccumInputs {
    InitExpMovingAvgInputs(SbExpr inputExpr) : inputExpr(std::move(inputExpr)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
};

struct InitIntegralInputs : public AccumInputs {
    InitIntegralInputs(SbExpr inputExpr) : inputExpr(std::move(inputExpr)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
};

struct FinalizeTopBottomNInputs : public AccumInputs {
    FinalizeTopBottomNInputs(SbExpr sortSpec) : sortSpec(std::move(sortSpec)) {}

    AccumInputsPtr clone() const final;

    SbExpr sortSpec;
};

struct FinalizeDerivativeInputs : public AccumInputs {
    FinalizeDerivativeInputs(
        SbExpr unit, SbExpr inputFirst, SbExpr sortByFirst, SbExpr inputLast, SbExpr sortByLast)
        : unit(std::move(unit)),
          inputFirst(std::move(inputFirst)),
          sortByFirst(std::move(sortByFirst)),
          inputLast(std::move(inputLast)),
          sortByLast(std::move(sortByLast)) {}

    AccumInputsPtr clone() const final;

    SbExpr unit;
    SbExpr inputFirst;
    SbExpr sortByFirst;
    SbExpr inputLast;
    SbExpr sortByLast;
};

struct FinalizeLinearFillInputs : public AccumInputs {
    FinalizeLinearFillInputs(SbExpr sortBy) : sortBy(std::move(sortBy)) {}

    AccumInputsPtr clone() const final;

    SbExpr sortBy;
};

struct FinalizeWindowFirstLastInputs : public AccumInputs {
    FinalizeWindowFirstLastInputs(SbExpr inputExpr, SbExpr defaultVal)
        : inputExpr(std::move(inputExpr)), defaultVal(std::move(defaultVal)) {}

    AccumInputsPtr clone() const final;

    SbExpr inputExpr;
    SbExpr defaultVal;
};

struct CombineAggsTopBottomNInputs : public AccumInputs {
    CombineAggsTopBottomNInputs(SbExpr sortSpec) : sortSpec(std::move(sortSpec)) {}

    AccumInputsPtr clone() const final;

    SbExpr sortSpec;
};
}  // namespace mongo::stage_builder
