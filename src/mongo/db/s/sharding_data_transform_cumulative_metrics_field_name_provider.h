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

#include "mongo/base/string_data.h"

namespace mongo {

class ShardingDataTransformCumulativeMetricsFieldNameProvider {
public:
    virtual ~ShardingDataTransformCumulativeMetricsFieldNameProvider() = default;
    StringData getForCountStarted() const;
    StringData getForCountSucceeded() const;
    StringData getForCountFailed() const;
    StringData getForCountCanceled() const;
    StringData getForLastOpEndingChunkImbalance() const;
    virtual StringData getForDocumentsProcessed() const = 0;
    virtual StringData getForBytesWritten() const = 0;
    StringData getForCountWritesToStashCollections() const;
    StringData getForCountWritesDuringCriticalSection() const;
    StringData getForCountReadsDuringCriticalSection() const;
    StringData getForCoordinatorAllShardsLowestRemainingOperationTimeEstimatedMillis() const;
    StringData getForCoordinatorAllShardsHighestRemainingOperationTimeEstimatedMillis() const;
    StringData getForRecipientRemainingOperationTimeEstimatedMillis() const;
    StringData getForCollectionCloningTotalRemoteBatchRetrievalTimeMillis() const;
    StringData getForCollectionCloningTotalRemoteBatchesRetrieved() const;
    StringData getForCollectionCloningTotalLocalInsertTimeMillis() const;
    StringData getForCollectionCloningTotalLocalInserts() const;
};

class ShardingDataTransformCumulativeMetricsFieldNamePlaceholder
    : public ShardingDataTransformCumulativeMetricsFieldNameProvider {
public:
    virtual ~ShardingDataTransformCumulativeMetricsFieldNamePlaceholder() = default;
    virtual StringData getForDocumentsProcessed() const override;
    virtual StringData getForBytesWritten() const override;
};

}  // namespace mongo
