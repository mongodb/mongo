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

#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/exception_util_gen.h"
#include "mongo/db/storage/storage_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWiredTiger

namespace mongo {

/**
 * Configured WT cache is deemed insufficient for a transaction when its dirty bytes in cache
 * exceed a certain threshold on the proportion of total cache which is used by transaction.
 *
 * For instance, if the transaction uses 80% of WT cache and the threshold is set to 75%, the
 * transaction is considered too large.
 */
bool cacheIsInsufficientForTransaction(WT_SESSION* session, double threshold) {
    StatusWith<int64_t> txnDirtyBytes = WiredTigerUtil::getStatisticsValue(
        session, "statistics:session", "", WT_STAT_SESSION_TXN_BYTES_DIRTY);
    if (!txnDirtyBytes.isOK()) {
        tasserted(6190900,
                  str::stream() << "unable to gather the WT session's txn dirty bytes: "
                                << txnDirtyBytes.getStatus());
    }

    StatusWith<int64_t> cacheDirtyBytes = WiredTigerUtil::getStatisticsValue(
        session, "statistics:", "", WT_STAT_CONN_CACHE_BYTES_DIRTY);
    if (!cacheDirtyBytes.isOK()) {
        tasserted(6190901,
                  str::stream() << "unable to gather the WT connection's cache dirty bytes: "
                                << txnDirtyBytes.getStatus());
    }

    return txnExceededCacheThreshold(
        txnDirtyBytes.getValue(), cacheDirtyBytes.getValue(), threshold);
}

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

str::stream generateContextStrStream(StringData prefix, StringData reason, int retCode) {
    str::stream contextStrStream;
    if (!prefix.empty())
        contextStrStream << prefix << " ";
    contextStrStream << retCode << ": " << reason;

    return contextStrStream;
};

bool rollbackReasonWasCachePressure(const char* reason) {
    return reason &&
        (strncmp(WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION,
                 reason,
                 sizeof(WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION)) == 0 ||
         strncmp(WT_TXN_ROLLBACK_REASON_CACHE_OVERFLOW,
                 reason,
                 sizeof(WT_TXN_ROLLBACK_REASON_CACHE_OVERFLOW)) == 0);
}

void throwAppropriateException(bool txnTooLargeEnabled,
                               bool temporarilyUnavailableEnabled,
                               bool cacheIsInsufficientForTransaction,
                               const char* reason,
                               StringData prefix,
                               int retCode) {

    if ((txnTooLargeEnabled || temporarilyUnavailableEnabled) &&
        rollbackReasonWasCachePressure(reason)) {
        if (txnTooLargeEnabled && cacheIsInsufficientForTransaction) {
            throwTransactionTooLargeForCache(
                generateContextStrStream(
                    prefix, WT_TXN_ROLLBACK_REASON_TOO_LARGE_FOR_CACHE, retCode)
                << " (" << reason << ")");
        }

        if (temporarilyUnavailableEnabled) {
            throwTemporarilyUnavailableException(generateContextStrStream(prefix, reason, retCode));
        }
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
        const char* reason = session ? session->get_rollback_reason(session) : "";

        throwAppropriateException(txnTooLargeEnabled,
                                  temporarilyUnavailableEnabled,
                                  cacheIsInsufficientForTransaction(session, cacheThreshold),
                                  reason,
                                  prefix,
                                  retCode);
    }

    // Don't abort on WT_PANIC when repairing, as the error will be handled at a higher layer.
    fassert(28559, retCode != WT_PANIC || storageGlobalParams.repair);

    auto s = generateContextStrStream(prefix, wiredtiger_strerror(retCode), retCode);

    if (retCode == EINVAL) {
        return Status(ErrorCodes::BadValue, s);
    }
    if (retCode == EMFILE) {
        return Status(ErrorCodes::TooManyFilesOpen, s);
    }
    if (retCode == EBUSY) {
        return Status(ErrorCodes::ObjectIsBusy, s);
    }

    uassert(ErrorCodes::ExceededMemoryLimit, s, retCode != WT_CACHE_FULL);

    // TODO convert specific codes rather than just using UNKNOWN_ERROR for everything.
    return Status(ErrorCodes::UnknownError, s);
}

}  // namespace mongo
