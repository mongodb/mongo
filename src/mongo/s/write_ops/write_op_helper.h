/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/commands/query_cmd/bulk_write_parser.h"

namespace mongo {
namespace write_op_helpers {

bool isRetryErrCode(int errCode);

template <typename ErrorType, typename GetErrCodeFn>
bool errorsAllSame(const std::vector<ErrorType>& errorItems, GetErrCodeFn getCodeFn) {
    tassert(10412301, "Expected at least one error item", !errorItems.empty());

    auto errCode = getCodeFn(errorItems.front());
    return std::all_of(
        ++errorItems.begin(), errorItems.end(), [errCode, &getCodeFn](const ErrorType& errorItem) {
            return getCodeFn(errorItem) == errCode;
        });
}

template <typename ErrorType, typename GetErrCodeFn>
bool hasOnlyOneNonRetryableError(const std::vector<ErrorType>& errorItems, GetErrCodeFn getCodeFn) {
    return std::count_if(
               errorItems.begin(), errorItems.end(), [&getCodeFn](const ErrorType& errorItem) {
                   return !isRetryErrCode(getCodeFn(errorItem));
               }) == 1;
}

template <typename ErrorType, typename GetErrCodeFn>
bool hasAnyNonRetryableError(const std::vector<ErrorType>& errorItems, GetErrCodeFn getCodeFn) {
    return std::count_if(
               errorItems.begin(), errorItems.end(), [&getCodeFn](const ErrorType& errorItem) {
                   return !isRetryErrCode(getCodeFn(errorItem));
               }) > 0;
}

template <typename ErrorType, typename GetErrCodeFn>
ErrorType getFirstNonRetryableError(const std::vector<ErrorType>& errorItems,
                                    GetErrCodeFn getCodeFn) {
    auto nonRetryableErr = std::find_if(
        errorItems.begin(), errorItems.end(), [&getCodeFn](const ErrorType& errorItem) {
            return !isRetryErrCode(getCodeFn(errorItem));
        });
    tassert(10412307, "No non-retryable error found", nonRetryableErr != errorItems.end());
    return *nonRetryableErr;
}

/**
 * Returns whether an operation should target all shards with ShardVersion::IGNORED(). This is
 * true for multi: true writes where 'onlyTargetDataOwningShardsForMultiWrites' is false and we are
 * not in a transaction.
 */
bool shouldTargetAllShardsSVIgnored(bool inTransaction, bool isMulti);


/**
 * Used to check if a partially applied (successful on some shards but not others)operation has an
 * errors that is safe to ignore. UUID mismatch errors are safe to ignore if the actualCollection is
 * null in conjuntion with other successful operations. This is true because it means we wrongly
 * targeted a non-owning shard with the operation and we wouldn't have applied any modifications
 * anyway. Note this is only safe if we're using ShardVersion::IGNORED since we're ignoring any
 * placement concern and broadcasting to all shards.
 */
bool isSafeToIgnoreErrorInPartiallyAppliedOp(const Status& status);

}  // namespace write_op_helpers
}  // namespace mongo
