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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/stage_types.h"

namespace mongo::stage_builder {

/**
 * Creates an EFunction expression with the given name and arguments.
 */
inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name,
                                                      sbe::EExpression::Vector args) {
    return sbe::makeE<sbe::EFunction>(name, std::move(args));
}

template <typename... Args>
inline std::unique_ptr<sbe::EExpression> makeFunction(StringData name, Args&&... args) {
    return sbe::makeE<sbe::EFunction>(name, sbe::makeEs(std::forward<Args>(args)...));
}

inline auto makeConstant(sbe::value::TypeTags tag, sbe::value::Value val) {
    return sbe::makeE<sbe::EConstant>(tag, val);
}

inline auto makeNothingConstant() {
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0);
}
inline auto makeNullConstant() {
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Null, 0);
}
inline auto makeBoolConstant(bool boolVal) {
    auto val = sbe::value::bitcastFrom<bool>(boolVal);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean, val);
}
inline auto makeInt32Constant(int32_t num) {
    auto val = sbe::value::bitcastFrom<int32_t>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, val);
}
inline auto makeInt64Constant(int64_t num) {
    auto val = sbe::value::bitcastFrom<int64_t>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt64, val);
}
inline auto makeDoubleConstant(double num) {
    auto val = sbe::value::bitcastFrom<double>(num);
    return sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberDouble, val);
}
inline auto makeDecimalConstant(const Decimal128& num) {
    auto [tag, val] = sbe::value::makeCopyDecimal(num);
    return sbe::makeE<sbe::EConstant>(tag, val);
}
inline auto makeStrConstant(StringData str) {
    auto [tag, val] = sbe::value::makeNewString(str);
    return sbe::makeE<sbe::EConstant>(tag, val);
}

inline std::unique_ptr<sbe::EExpression> makeVariable(sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(slotId);
}

inline std::unique_ptr<sbe::EExpression> makeVariable(sbe::FrameId frameId,
                                                      sbe::value::SlotId slotId) {
    return sbe::makeE<sbe::EVariable>(frameId, slotId);
}

inline auto makeFail(int code, StringData errorMessage) {
    return sbe::makeE<sbe::EFail>(ErrorCodes::Error{code}, errorMessage);
}

std::unique_ptr<sbe::EExpression> makeIf(std::unique_ptr<sbe::EExpression> condExpr,
                                         std::unique_ptr<sbe::EExpression> thenExpr,
                                         std::unique_ptr<sbe::EExpression> elseExpr);

std::unique_ptr<sbe::EExpression> makeLet(sbe::FrameId frameId,
                                          sbe::EExpression::Vector bindExprs,
                                          std::unique_ptr<sbe::EExpression> expr);

std::unique_ptr<sbe::EExpression> makeLocalLambda(sbe::FrameId frameId,
                                                  std::unique_ptr<sbe::EExpression> expr);

/**
 * Creates a chain of EIf expressions that will inspect each arg in order and return the first
 * arg that is not null or missing.
 */
std::unique_ptr<sbe::EExpression> makeNumericConvert(std::unique_ptr<sbe::EExpression> expr,
                                                     sbe::value::TypeTags tag);

/** This helper takes an SBE SlotIdGenerator and an SBE Array and returns an output slot and a
 * unwind/project/limit/coscan subtree that streams out the elements of the array one at a time via
 * the output slot over a series of calls to getNext(), mimicking the output of a collection scan or
 * an index scan. Note that this method assumes ownership of the SBE Array being passed in.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateVirtualScan(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy = nullptr,
    PlanNodeId planNodeId = kEmptyPlanNodeId);

/**
 * Make a mock scan with multiple output slots from an BSON array. This method does NOT assume
 * ownership of the BSONArray passed in.
 */
std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> generateVirtualScanMulti(
    sbe::value::SlotIdGenerator* slotIdGenerator,
    int numSlots,
    sbe::value::TypeTags arrTag,
    sbe::value::Value arrVal,
    PlanYieldPolicy* yieldPolicy = nullptr,
    PlanNodeId planNodeId = kEmptyPlanNodeId);

}  // namespace mongo::stage_builder
