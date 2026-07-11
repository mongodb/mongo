// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/window_function/window_function_statement.h"
#include "mongo/db/query/stage_builder/sbe/gen_accumulator.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo::stage_builder {
struct WindowOpInfo;

class WindowOp {
public:
    WindowOp(std::string opName);

    WindowOp(std::string_view opName) : WindowOp(std::string{opName}) {}

    WindowOp(const WindowFunctionStatement& wf);

    std::string_view getOpName() const {
        return _opName;
    }

    /**
     * This method returns the number of agg expressions that need to be generated for this
     * WindowOp.
     */
    size_t getNumAggs() const;

    /**
     * Given one or more input expressions ('input' / 'inputs'), these methods generate the
     * arg expressions needed for this WindowOp.
     */
    AccumInputsPtr buildAddRemoveExprs(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given a vector of named arg expressions ('args' / 'argNames'), this method generates the
     * accumulate expressions for this WindowOp.
     */
    SbExpr::Vector buildAddAggs(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given a vector of named arg expressions ('args' / 'argNames'), this method generates the
     * accumulate expressions for this WindowOp.
     */
    SbExpr::Vector buildRemoveAggs(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given a map of input expressions ('argExprs'), these methods generate the initialize
     * expressions for this WindowOp.
     */
    SbExpr::Vector buildInitialize(StageBuilderState& state, AccumInputsPtr inputs) const;

    /**
     * Given a map of input expressions ('argExprs'), this method generates the finalize
     * expression for this WindowOp.
     */
    SbExpr buildFinalize(StageBuilderState& state,
                         AccumInputsPtr inputs,
                         const SbSlotVector& aggSlots) const;

private:
    // Static helper method for looking up the info for this WindowOp in the global map.
    // This method should only be used by WindowOp's constructors.
    static const WindowOpInfo* lookupOpInfo(const std::string& opName);

    // Non-static checked helper method for retrieving the value of '_opInfo'. This method will
    // raise a tassert if '_opInfo' is null.
    const WindowOpInfo* getOpInfo() const {
        uassert(8859901,
                str::stream() << "Unrecognized WindowOp name: " << _opName,
                _opInfo != nullptr);

        return _opInfo;
    }

    // Name of the specific accumulation op. This name is used to retrieve info about the op
    // from the global map.
    std::string _opName;

    // Info about the specific accumulation op named by '_opName'.
    const WindowOpInfo* _opInfo = nullptr;
};
}  // namespace mongo::stage_builder
