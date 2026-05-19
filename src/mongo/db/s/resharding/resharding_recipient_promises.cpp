/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_recipient_promises.h"

#include <utility>

namespace mongo {

ReshardingRecipientPromises::ReshardingRecipientPromises()
    : _allDonorsPreparedToDonate(_registry,
                                 [this](WithLock lk, const ReshardingRecipientDocument& doc) {
                                     _recoverAllDonorsPreparedToDonate(lk, doc);
                                 }) {}

void ReshardingRecipientPromises::recover(WithLock lk, const ReshardingRecipientDocument& doc) {
    _registry.recover(lk, doc);
}

void ReshardingRecipientPromises::setError(WithLock lk, Status status) {
    _registry.setError(lk, status);
}

void ReshardingRecipientPromises::onCoordinatorStateAdvanced(
    WithLock lk, CoordinatorStateEnum newState, boost::optional<CloneDetails> cloneDetails) {
    if (cloneDetails && newState >= CoordinatorStateEnum::kCloning) {
        _allDonorsPreparedToDonate.emplaceValue(lk, std::move(*cloneDetails));
    }
}

SharedSemiFuture<ReshardingRecipientPromises::CloneDetails>
ReshardingRecipientPromises::getAllDonorsPreparedToDonateFuture() const {
    return _allDonorsPreparedToDonate.getFuture();
}

void ReshardingRecipientPromises::_recoverAllDonorsPreparedToDonate(
    WithLock lk, const ReshardingRecipientDocument& doc) {
    if (doc.getMutableState().getState() <= RecipientStateEnum::kAwaitingFetchTimestamp) {
        return;
    }
    auto cloneTimestamp = doc.getCloneTimestamp();
    auto metrics = doc.getMetrics();
    if (!cloneTimestamp || !metrics) {
        return;
    }
    if (!metrics->getApproxDocumentsToCopy() || !metrics->getApproxBytesToCopy()) {
        return;
    }
    _allDonorsPreparedToDonate.emplaceValue(lk,
                                            CloneDetails{*cloneTimestamp,
                                                         *metrics->getApproxDocumentsToCopy(),
                                                         *metrics->getApproxBytesToCopy(),
                                                         doc.getDonorShards()});
}

}  // namespace mongo
