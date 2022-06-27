/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery
constexpr bool kMongoOptimizerStdCoutDebugOutput = false;

namespace mongo {

template <typename T>
void coutPrintAttr(const logv2::detail::NamedArg<T>& arg) {
    std::cout << arg.name << " : " << arg.value << "\n";
}

template <typename T, typename... Args>
void coutPrintAttr(const logv2::detail::NamedArg<T>& arg,
                   const logv2::detail::NamedArg<Args>&... args) {
    std::cout << arg.name << " : " << arg.value << "\n";
    coutPrintAttr(args...);
}

template <typename... Args>
void coutPrint(const std::string& msg, const logv2::detail::NamedArg<Args>&... args) {
    std::cout << "********* " << msg << " *********\n";
    coutPrintAttr(args...);
    std::cout << "********* " << msg << " *********\n";
}

#define OPTIMIZER_DEBUG_LOG(ID, DLEVEL, FMTSTR_MESSAGE, ...) \
    LOGV2_DEBUG(ID, DLEVEL, FMTSTR_MESSAGE, ##__VA_ARGS__);  \
    if (kMongoOptimizerStdCoutDebugOutput)                   \
        ::mongo::coutPrint(FMTSTR_MESSAGE, __VA_ARGS__);

/**
 * Returns whether the given Pipeline and aggregate command is eligible to use the bonsai
 * optimizer.
 */
bool isEligibleForBonsai(const AggregateCommandRequest& request,
                         const Pipeline& pipeline,
                         OperationContext* opCtx,
                         const CollectionPtr& collection);

/**
 * Returns whether the given find command is eligible to use the bonsai optimizer.
 */
bool isEligibleForBonsai(const FindCommandRequest& request,
                         const MatchExpression& expression,
                         OperationContext* opCtx,
                         const CollectionPtr& collection);

}  // namespace mongo
#undef MONGO_LOGV2_DEFAULT_COMPONENT
