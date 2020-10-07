/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder_helpers.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/matcher/matcher_type_set.h"

namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicOr,
        sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot,
                                    sbe::makeE<sbe::EFunction>("exists", sbe::makeEs(var.clone()))),
        sbe::makeE<sbe::EFunction>("isNull", sbe::makeEs(var.clone())));
}

std::unique_ptr<sbe::EExpression> generateNullOrMissing(const sbe::FrameId frameId,
                                                        const sbe::value::SlotId slotId) {
    sbe::EVariable var{frameId, slotId};
    return generateNullOrMissing(var);
}

std::unique_ptr<sbe::EExpression> generateNonNumericCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isNumber", sbe::makeEs(var.clone())));
}

std::unique_ptr<sbe::EExpression> generateLongLongMinCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::logicAnd,
        sbe::makeE<sbe::ETypeMatch>(var.clone(),
                                    MatcherTypeSet{BSONType::NumberLong}.getBSONTypeMask()),
        sbe::makeE<sbe::EPrimBinary>(
            sbe::EPrimBinary::eq,
            var.clone(),
            sbe::makeE<sbe::EConstant>(
                sbe::value::TypeTags::NumberInt64,
                sbe::value::bitcastFrom<int64_t>(std::numeric_limits<int64_t>::min()))));
}

std::unique_ptr<sbe::EExpression> generateNaNCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EFunction>("isNaN", sbe::makeEs(var.clone()));
}

std::unique_ptr<sbe::EExpression> generateNonPositiveCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::EPrimBinary::lessEq,
        var.clone(),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generateNegativeCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimBinary>(
        sbe::EPrimBinary::EPrimBinary::less,
        var.clone(),
        sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32,
                                   sbe::value::bitcastFrom<int32_t>(0)));
}

std::unique_ptr<sbe::EExpression> generateNonObjectCheck(const sbe::EVariable& var) {
    return sbe::makeE<sbe::EPrimUnary>(
        sbe::EPrimUnary::logicNot,
        sbe::makeE<sbe::EFunction>("isObject", sbe::makeEs(var.clone())));
}

template <>
std::unique_ptr<sbe::EExpression> buildMultiBranchConditional(
    std::unique_ptr<sbe::EExpression> defaultCase) {
    return defaultCase;
}

std::unique_ptr<sbe::PlanStage> makeLimitCoScanTree(PlanNodeId planNodeId, long long limit) {
    return sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(planNodeId), limit, boost::none, planNodeId);
}

std::unique_ptr<sbe::EExpression> makeNot(std::unique_ptr<sbe::EExpression> e) {
    return sbe::makeE<sbe::EPrimUnary>(sbe::EPrimUnary::logicNot, std::move(e));
}

std::unique_ptr<sbe::EExpression> makeFillEmptyFalse(std::unique_ptr<sbe::EExpression> e) {
    using namespace std::literals;
    return sbe::makeE<sbe::EFunction>(
        "fillEmpty"sv,
        sbe::makeEs(std::move(e),
                    sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                               sbe::value::bitcastFrom<bool>(false))));
}

}  // namespace mongo::stage_builder
