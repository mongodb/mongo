// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <wiredtiger.h>

namespace mongo {

class WiredTigerSession;

bool txnExceededCacheThreshold(int64_t txnDirtyBytes, int64_t cacheDirtyBytes, double threshold);
bool rollbackReasonWasCachePressure(int sub_level_err);
void throwCachePressureExceptionIfAppropriate(bool txnTooLargeEnabled,
                                              bool cacheIsInsufficientForTransaction,
                                              const char* reason,
                                              std::string_view prefix,
                                              int retCode);
void throwAppropriateException(bool txnTooLargeEnabled,
                               WT_SESSION* session,
                               double cacheThreshold,
                               std::string_view prefix,
                               int retCode);

/**
 * Dumps the origin of an error code and the stacktrace from WiredTiger.
 */
void dumpErrorLog(int retCode);

Status wtRCToStatus_slow(int retCode, WT_SESSION* session, std::string_view prefix);
Status wtRCToStatus_slow(int retCode, WiredTigerSession& session, std::string_view prefix);

inline Status wtRCToStatus_error(int retCode, WT_SESSION* session, const char* prefix = nullptr) {
    invariant(retCode != 0);
    dumpErrorLog(retCode);
    return wtRCToStatus_slow(retCode, session, prefix ? prefix : std::string_view{});
}

inline Status wtRCToStatus_error(int retCode,
                                 WiredTigerSession& session,
                                 const char* prefix = nullptr) {
    invariant(retCode != 0);
    dumpErrorLog(retCode);
    return wtRCToStatus_slow(retCode, session, prefix ? prefix : std::string_view{});
}

/**
 * converts wiredtiger return codes to mongodb statuses.
 */
inline Status wtRCToStatus(int retCode, WT_SESSION* session, const char* prefix = nullptr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, prefix ? prefix : std::string_view{});
}

inline Status wtRCToStatus(int retCode, WiredTigerSession& session, const char* prefix = nullptr) {
    if (MONGO_likely(retCode == 0))
        return Status::OK();

    return wtRCToStatus_slow(retCode, session, prefix ? prefix : std::string_view{});
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

#define MONGO_invariantWTOK_2(expression, session)                                                \
    do {                                                                                          \
        int _invariantWTOK_retCode = expression;                                                  \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {                                        \
            error_details::invariantOKFailed(#expression,                                         \
                                             wtRCToStatus_error(_invariantWTOK_retCode, session), \
                                             MONGO_SOURCE_LOCATION());                            \
        }                                                                                         \
    } while (false)

#define MONGO_invariantWTOK_3(expression, session, contextExpr)      \
    do {                                                             \
        int _invariantWTOK_retCode = expression;                     \
        if (MONGO_unlikely(_invariantWTOK_retCode != 0)) {           \
            error_details::invariantOKFailedWithMsg(                 \
                #expression,                                         \
                wtRCToStatus_error(_invariantWTOK_retCode, session), \
                contextExpr,                                         \
                MONGO_SOURCE_LOCATION());                            \
        }                                                            \
    } while (false)

#define MONGO_invariantWTOK_EXPAND(x) x /**< MSVC workaround */
#define MONGO_invariantWTOK_PICK(_1, _2, _3, x, ...) x
#define invariantWTOK(...)                                                                 \
    MONGO_invariantWTOK_EXPAND(MONGO_invariantWTOK_PICK(                                   \
        __VA_ARGS__, MONGO_invariantWTOK_3, MONGO_invariantWTOK_2, MONGO_invariantWTOK_1)( \
        __VA_ARGS__))

}  // namespace mongo
