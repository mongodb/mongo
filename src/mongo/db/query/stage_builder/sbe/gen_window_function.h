/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_statement.h"
#include "mongo/db/query/stage_builder/sbe/gen_accumulator.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

namespace mongo::stage_builder {
struct WindowOpInfo;

class WindowOp {
public:
    WindowOp(std::string opName);

    WindowOp(StringData opName) : WindowOp(std::string{opName}) {}

    WindowOp(const WindowFunctionStatement& wf);

    StringData getOpName() const {
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
