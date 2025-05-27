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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

#include <wiredtiger.h>

namespace mongo {

class WiredTigerSession;

bool txnExceededCacheThreshold(int64_t txnDirtyBytes, int64_t cacheDirtyBytes, double threshold);
bool rollbackReasonWasCachePressure(int sub_level_err);
void throwCachePressureExceptionIfAppropriate(bool txnTooLargeEnabled,
                                              bool temporarilyUnavailableEnabled,
                                              bool cacheIsInsufficientForTransaction,
                                              const char* reason,
                                              StringData prefix,
                                              int retCode);
void throwAppropriateException(bool txnTooLargeEnabled,
                               bool temporarilyUnavailableEnabled,
                               WT_SESSION* session,
                               double cacheThreshold,
                               StringData prefix,
                               int retCode);
Status wtRCToStatus_slow(int retCode, WT_SESSION* session, StringData prefix);
Status wtRCToStatus_slow(int retCode, WiredTigerSession& session, StringData prefix);

/**
 * converts wiredtiger return codes to mongodb statuses.
 */
inline Status wtRCToStatus(int retCode, WT_SESSION* session, const char* prefix = nullptr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, prefix);
}

inline Status wtRCToStatus(int retCode, WiredTigerSession& session, const char* prefix = nullptr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, prefix);
}

template <typename ContextExpr>
Status wtRCToStatus(int retCode, WT_SESSION* session, ContextExpr&& contextExpr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, std::forward<ContextExpr>(contextExpr)());
}

template <typename ContextExpr>
Status wtRCToStatus(int retCode, WiredTigerSession& session, ContextExpr&& contextExpr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, std::forward<ContextExpr>(contextExpr)());
}

inline void uassertWTOK(int ret, WT_SESSION* session) {
    uassertStatusOK(wtRCToStatus(ret, session));
}

#define MONGO_invariantWTOK_2(expression, session)                           \
    do {                                                                     \
        int _invariantWTOK_retCode = expression;                             \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {                   \
            invariantOKFailed(#expression,                                   \
                              wtRCToStatus(_invariantWTOK_retCode, session), \
                              MONGO_SOURCE_LOCATION());                      \
        }                                                                    \
    } while (false)

#define MONGO_invariantWTOK_3(expression, session, contextExpr)                     \
    do {                                                                            \
        int _invariantWTOK_retCode = expression;                                    \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {                          \
            invariantOKFailedWithMsg(#expression,                                   \
                                     wtRCToStatus(_invariantWTOK_retCode, session), \
                                     contextExpr,                                   \
                                     MONGO_SOURCE_LOCATION());                      \
        }                                                                           \
    } while (false)

#define MONGO_invariantWTOK_EXPAND(x) x /**< MSVC workaround */
#define MONGO_invariantWTOK_PICK(_1, _2, _3, x, ...) x
#define invariantWTOK(...)                                                                 \
    MONGO_invariantWTOK_EXPAND(MONGO_invariantWTOK_PICK(                                   \
        __VA_ARGS__, MONGO_invariantWTOK_3, MONGO_invariantWTOK_2, MONGO_invariantWTOK_1)( \
        __VA_ARGS__))

}  // namespace mongo
