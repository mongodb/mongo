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

#include "mongo/db/s/resharding/resharding_coordinator_observer.h"

#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_id.h"

namespace mongo {

ReshardingCoordinatorObserver::ReshardingCoordinatorObserver() = default;

ReshardingCoordinatorObserver::~ReshardingCoordinatorObserver() = default;

// TODO SERVER-49572
void ReshardingCoordinatorObserver::onRecipientReportsCreatedCollection(const ShardId& recipient) {}

// TODO SERVER-49573
void ReshardingCoordinatorObserver::onDonorReportsMinFetchTimestamp(const ShardId& donor,
                                                                    Timestamp timestamp) {}

// TODO SERVER-49574
void ReshardingCoordinatorObserver::onRecipientFinishesCloning(const ShardId& recipient) {}

// TODO SERVER-49575
void ReshardingCoordinatorObserver::onRecipientReportsStrictlyConsistent(const ShardId& recipient) {
}

// TODO SERVER-49576
void ReshardingCoordinatorObserver::onRecipientRenamesCollection(const ShardId& recipient) {}

// TODO SERVER-49577
void ReshardingCoordinatorObserver::onDonorDropsOriginalCollection(const ShardId& donor) {}

// TODO SERVER-49578
void ReshardingCoordinatorObserver::onRecipientReportsUnrecoverableError(const ShardId& recipient,
                                                                         Status error) {}

// TODO	SERVER-49572
SharedSemiFuture<void> ReshardingCoordinatorObserver::awaitAllRecipientsCreatedCollection() {
    return _allRecipientsCreatedCollection.getFuture();
}

// TODO SERVER-49573
SharedSemiFuture<Timestamp> ReshardingCoordinatorObserver::awaitAllDonorsReadyToDonate() {
    return _allDonorsReportedMinFetchTimestamp.getFuture();
}

// TODO SERVER-49574
SharedSemiFuture<void> ReshardingCoordinatorObserver::awaitAllRecipientsFinishedCloning() {
    return _allRecipientsFinishedCloning.getFuture();
}

// TODO SERVER-49575
SharedSemiFuture<void> ReshardingCoordinatorObserver::awaitAllRecipientsInStrictConsistency() {
    return _allRecipientsReportedStrictConsistencyTimestamp.getFuture();
}

// TODO SERVER-49577
SharedSemiFuture<void> ReshardingCoordinatorObserver::awaitAllDonorsDroppedOriginalCollection() {
    return _allDonorsDroppedOriginalCollection.getFuture();
}

// TODO SERVER-49576
SharedSemiFuture<void> ReshardingCoordinatorObserver::awaitAllRecipientsRenamedCollection() {
    return _allRecipientsRenamedCollection.getFuture();
}

}  // namespace mongo
