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

#include "mongo/db/query/sbe_stage_builder_window_function.h"
#include "mongo/db/query/sbe_stage_builder_accumulator.h"

namespace mongo::stage_builder {

template <int N>
std::vector<std::unique_ptr<sbe::EExpression>> emptyInitializer() {
    return std::vector<std::unique_ptr<sbe::EExpression>>{N};
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAddSum(
    std::unique_ptr<sbe::EExpression> arg) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableSumAdd", std::move(arg)));
    return exprs;
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemoveSum(
    std::unique_ptr<sbe::EExpression> arg) {
    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.push_back(makeFunction("aggRemovableSumRemove", std::move(arg)));
    return exprs;
}

std::unique_ptr<sbe::EExpression> buildWindowFinalizeSum(sbe::value::SlotVector slots) {
    sbe::EExpression::Vector exprs;
    for (auto slot : slots) {
        exprs.push_back(makeVariable(slot));
    }
    return makeE<sbe::EFunction>("aggRemovableSumFinalize", std::move(exprs));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowInit(
    const WindowFunctionStatement& stmt) {
    using BuildInitFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>()>;

    static const StringDataMap<BuildInitFn> kWindowFunctionBuilders = {
        {"$sum", &emptyInitializer<1>},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914603,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowAdd(
    const WindowFunctionStatement& stmt, std::unique_ptr<sbe::EExpression> arg) {
    using BuildAddFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        std::unique_ptr<sbe::EExpression>)>;

    static const StringDataMap<BuildAddFn> kWindowFunctionBuilders = {
        {"$sum", &buildWindowAddSum},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914604,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), std::move(arg));
}

std::vector<std::unique_ptr<sbe::EExpression>> buildWindowRemove(
    const WindowFunctionStatement& stmt, std::unique_ptr<sbe::EExpression> arg) {
    using BuildRemoveFn = std::function<std::vector<std::unique_ptr<sbe::EExpression>>(
        std::unique_ptr<sbe::EExpression>)>;

    static const StringDataMap<BuildRemoveFn> kWindowFunctionBuilders = {
        {"$sum", &buildWindowRemoveSum},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914605,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), std::move(arg));
}

std::unique_ptr<sbe::EExpression> buildWindowFinalize(const WindowFunctionStatement& stmt,
                                                      sbe::value::SlotVector values) {
    using BuildFinalizeFn =
        std::function<std::unique_ptr<sbe::EExpression>(sbe::value::SlotVector values)>;

    static const StringDataMap<BuildFinalizeFn> kWindowFunctionBuilders = {
        {"$sum", &buildWindowFinalizeSum},
    };

    auto opName = stmt.expr->getOpName();
    uassert(7914606,
            str::stream() << "Unsupported window function in SBE stage builder: " << opName,
            kWindowFunctionBuilders.find(opName) != kWindowFunctionBuilders.end());

    return std::invoke(kWindowFunctionBuilders.at(opName), std::move(values));
}
}  // namespace mongo::stage_builder
