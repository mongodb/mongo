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

#include "mongo/db/s/resharding/resharding_metrics_field_name_provider.h"

namespace mongo {
namespace {

constexpr auto kBytesCopied = "bytesCopied";
constexpr auto kDocumentsCopied = "documentsCopied";
constexpr auto kInsertsApplied = "insertsApplied";
constexpr auto kUpdatesApplied = "updatesApplied";
constexpr auto kDeletesApplied = "deletesApplied";
constexpr auto kOplogEntriesApplied = "oplogEntriesApplied";
constexpr auto kOplogEntriesFetched = "oplogEntriesFetched";
constexpr auto kApplyTimeElapsed = "totalApplyTimeElapsedSecs";
constexpr auto kApproxDocumentsToCopy = "approxDocumentsToCopy";
constexpr auto kApproxBytesToCopy = "approxBytesToCopy";

}  // namespace

StringData ReshardingMetricsFieldNameProvider::getForBytesWritten() const {
    return kBytesCopied;
}

StringData ReshardingMetricsFieldNameProvider::getForDocumentsProcessed() const {
    return kDocumentsCopied;
}

StringData ReshardingMetricsFieldNameProvider::getForApproxDocumentsToProcess() const {
    return kApproxDocumentsToCopy;
}

StringData ReshardingMetricsFieldNameProvider::getForInsertsApplied() const {
    return kInsertsApplied;
}

StringData ReshardingMetricsFieldNameProvider::getForUpdatesApplied() const {
    return kUpdatesApplied;
}

StringData ReshardingMetricsFieldNameProvider::getForDeletesApplied() const {
    return kDeletesApplied;
}

StringData ReshardingMetricsFieldNameProvider::getForOplogEntriesApplied() const {
    return kOplogEntriesApplied;
}

StringData ReshardingMetricsFieldNameProvider::getForOplogEntriesFetched() const {
    return kOplogEntriesFetched;
}

StringData ReshardingMetricsFieldNameProvider::getForApplyTimeElapsed() const {
    return kApplyTimeElapsed;
}

StringData ReshardingMetricsFieldNameProvider::getForApproxBytesToScan() const {
    return kApproxBytesToCopy;
}
}  // namespace mongo
