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

#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

namespace mongo {

namespace {
static constexpr auto kTransactionTooLargeForCache =
    "transaction is too large and will not fit in the storage engine cache"_sd;
/**
 * Configured WT cache is deemed insufficient for a transaction when its dirty bytes in cache
 * exceed a certain threshold on the proportion of total cache which is used by transaction.
 *
 * For instance, if the transaction uses 80% of WT cache and the threshold is set to 75%, the
 * transaction is considered too large.
 */
bool cacheIsInsufficientForTransaction(WT_SESSION* session, double threshold) {
    StatusWith<int64_t> txnDirtyBytes = WiredTigerUtil::getStatisticsValue_DoNotUse(
        session, "statistics:session", "", WT_STAT_SESSION_TXN_BYTES_DIRTY);
    if (!txnDirtyBytes.isOK()) {
        tasserted(6190900,
                  str::stream() << "unable to gather the WT session's txn dirty bytes: "
                                << txnDirtyBytes.getStatus());
    }

    StatusWith<int64_t> cacheDirtyBytes = WiredTigerUtil::getStatisticsValue_DoNotUse(
        session, "statistics:", "", WT_STAT_CONN_CACHE_BYTES_DIRTY_LEAF);
    if (!cacheDirtyBytes.isOK()) {
        tasserted(6190901,
                  str::stream() << "unable to gather the WT connection's cache dirty bytes: "
                                << txnDirtyBytes.getStatus());
    }

    return txnExceededCacheThreshold(
        txnDirtyBytes.getValue(), cacheDirtyBytes.getValue(), threshold);
}

str::stream generateContextStrStream(StringData prefix, StringData reason, int retCode) {
    str::stream contextStrStream;
    if (!prefix.empty())
        contextStrStream << prefix << " ";
    contextStrStream << retCode << ": " << reason;

    return contextStrStream;
};
}  // namespace

bool txnExceededCacheThreshold(int64_t txnDirtyBytes, int64_t cacheDirtyBytes, double threshold) {
    double txnBytesDirtyOverCacheBytesDirty = static_cast<double>(txnDirtyBytes) / cacheDirtyBytes;

    LOGV2_DEBUG(6190902,
                2,
                "Checking if transaction can eventually succeed",
                "txnDirtyBytes"_attr = txnDirtyBytes,
                "cacheDirtyBytes"_attr = cacheDirtyBytes,
                "txnBytesDirtyOverCacheBytesDirty"_attr = txnBytesDirtyOverCacheBytesDirty,
                "threshold"_attr = threshold);

    return txnBytesDirtyOverCacheBytesDirty > threshold;
}

bool rollbackReasonWasCachePressure(int sub_level_err) {
    return sub_level_err == WT_CACHE_OVERFLOW || sub_level_err == WT_OLDEST_FOR_EVICTION;
}

void throwCachePressureExceptionIfAppropriate(bool txnTooLargeEnabled,
                                              bool temporarilyUnavailableEnabled,
                                              bool cacheIsInsufficientForTransaction,
                                              const char* reason,
                                              StringData prefix,
                                              int retCode) {
    if (txnTooLargeEnabled && cacheIsInsufficientForTransaction) {
        throwTransactionTooLargeForCache(
            generateContextStrStream(prefix, kTransactionTooLargeForCache, retCode)
            << " (" << reason << ")");
    }

    if (temporarilyUnavailableEnabled) {
        throwTemporarilyUnavailableException(generateContextStrStream(prefix, reason, retCode));
    }
}

void throwAppropriateException(bool txnTooLargeEnabled,
                               bool temporarilyUnavailableEnabled,
                               WT_SESSION* session,
                               double cacheThreshold,
                               StringData prefix,
                               int retCode) {

    // These values are initialized by WT_SESSION::get_last_error and should only be accessed if the
    // session is not null.
    int err = 0;
    int sub_level_err = WT_NONE;
    const char* reason = "";

    if (session) {
        session->get_last_error(session, &err, &sub_level_err, &reason);
    }

    if ((txnTooLargeEnabled || temporarilyUnavailableEnabled) &&
        rollbackReasonWasCachePressure(sub_level_err)) {
        throwCachePressureExceptionIfAppropriate(
            txnTooLargeEnabled,
            temporarilyUnavailableEnabled,
            cacheIsInsufficientForTransaction(session, cacheThreshold),
            reason,
            prefix,
            retCode);
    }

    throwWriteConflictException(prefix);
}

Status wtRCToStatus_slow(int retCode, WT_SESSION* session, StringData prefix) {
    if (retCode == 0)
        return Status::OK();

    if (retCode == WT_ROLLBACK) {
        double cacheThreshold = gTransactionTooLargeForCacheThreshold.load();
        bool txnTooLargeEnabled = cacheThreshold < 1.0;
        bool temporarilyUnavailableEnabled = gEnableTemporarilyUnavailableExceptions.load();

        throwAppropriateException(txnTooLargeEnabled,
                                  temporarilyUnavailableEnabled,
                                  session,
                                  cacheThreshold,
                                  prefix,
                                  retCode);
    }

    // Don't abort on WT_PANIC when repairing, as the error will be handled at a higher layer.
    fassert(28559, retCode != WT_PANIC || storageGlobalParams.repair);

    int err = 0;
    int subLevelErr = WT_NONE;
    const char* reason = "";
    const char* strerror = wiredtiger_strerror(retCode);

    if (session) {
        session->get_last_error(session, &err, &subLevelErr, &reason);
    }

    // Combine the sublevel err context with the generic context
    std::string errContext = std::string(strerror) + (reason ? " - " : "") + reason;
    auto s = generateContextStrStream(prefix, errContext.c_str(), retCode);

    if (retCode == EINVAL) {
        return Status(ErrorCodes::BadValue, s);
    }
    if (retCode == EMFILE) {
        return Status(ErrorCodes::TooManyFilesOpen, s);
    }
    if (retCode == EBUSY) {
        return Status(ErrorCodes::ObjectIsBusy, s);
    }
    if (retCode == EEXIST) {
        return Status(ErrorCodes::ObjectAlreadyExists, s);
    }
    if (retCode == ENOENT) {
        return Status(ErrorCodes::NoSuchKey, s);
    }

    uassert(ErrorCodes::ExceededMemoryLimit, s, retCode != WT_CACHE_FULL);

    return Status(ErrorCodes::UnknownError, s);
}

Status wtRCToStatus_slow(int retCode, WiredTigerSession& session, StringData prefix) {
    return session.with(
        [retCode, prefix](WT_SESSION* s) { return wtRCToStatus_slow(retCode, s, prefix); });
}

}  // namespace mongo
