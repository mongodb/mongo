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

#include "mongo/db/s/resharding/resharding_coordinator.h"

#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/shard_id.h"

namespace mongo {

namespace {
const auto getReshardingCoordinator = ServiceContext::declareDecoration<ReshardingCoordinator>();
}  // namespace

ReshardingCoordinator::ReshardingCoordinator() = default;

ReshardingCoordinator::~ReshardingCoordinator() {
    // TODO SERVER-49569 uncomment line below
    // invariant(_reshardingOperationsInProgress.empty());
}

ReshardingCoordinator& ReshardingCoordinator::get(ServiceContext* serviceContext) {
    return getReshardingCoordinator(serviceContext);
}

ReshardingCoordinator& ReshardingCoordinator::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

// TODO SERVER-49570
void ReshardingCoordinator::onNewReshardCollection(const NamespaceString& nss,
                                                   const std::vector<ShardId>& donors,
                                                   const std::vector<ShardId>& recipients,
                                                   const std::vector<ChunkType>& initialChunks) {}

// TODO	SERVER-49572
void ReshardingCoordinator::onRecipientReportsCreatedCollection(const NamespaceString& nss,
                                                                const ShardId& recipient) {}

// TODO SERVER-49573
void ReshardingCoordinator::onDonorReportsMinFetchTimestamp(const NamespaceString& nss,
                                                            const ShardId& donor,
                                                            Timestamp timestamp) {}

// TODO SERVER-49574
void ReshardingCoordinator::onRecipientFinishesCloning(const NamespaceString& nss,
                                                       const ShardId& recipient) {}

// TODO SERVER-49575
void ReshardingCoordinator::onRecipientReportsStrictlyConsistent(const NamespaceString& nss,
                                                                 const ShardId& recipient) {}

// TODO SERVER-49576
void ReshardingCoordinator::onRecipientRenamesCollection(const NamespaceString& nss,
                                                         const ShardId& recipient) {}

// TODO SERVER-49577
void ReshardingCoordinator::onDonorDropsOriginalCollection(const NamespaceString& nss,
                                                           const ShardId& donor) {}

// TODO SERVER-49578
void ReshardingCoordinator::onRecipientReportsUnrecoverableError(const NamespaceString& nss,
                                                                 const ShardId& recipient,
                                                                 Status error) {}
}  // namespace mongo
