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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_op_observer.h"

#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {

std::shared_ptr<ReshardingCoordinatorObserver> getReshardingCoordinatorObserver(
    OperationContext* opCtx, const BSONObj& reshardingId) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kReshardingCoordinatorServiceName);
    auto instance =
        ReshardingCoordinatorService::ReshardingCoordinator::lookup(opCtx, service, reshardingId);
    invariant(instance);
    return (*instance)->getObserver();
}

}  // namespace

ReshardingOpObserver::ReshardingOpObserver() = default;

ReshardingOpObserver::~ReshardingOpObserver() = default;

void ReshardingOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    // This is a no-op if either replication is not enabled or this node is a secondary
    if (!repl::ReplicationCoordinator::get(opCtx)->isReplEnabled() ||
        !opCtx->writesAreReplicated()) {
        return;
    }

    if (args.nss == NamespaceString::kConfigReshardingOperationsNamespace) {
        auto newCoordinatorDoc = ReshardingCoordinatorDocument::parse(
            IDLParserErrorContext("reshardingCoordinatorDoc"), args.updateArgs.updatedDoc);
        auto reshardingId =
            BSON(ReshardingCoordinatorDocument::k_idFieldName << newCoordinatorDoc.get_id());
        auto observer = getReshardingCoordinatorObserver(opCtx, reshardingId);
        opCtx->recoveryUnit()->onCommit(
            [observer = std::move(observer), newCoordinatorDoc = std::move(newCoordinatorDoc)](
                boost::optional<Timestamp> unusedCommitTime) mutable {
                observer->onReshardingParticipantTransition(newCoordinatorDoc);
            });
    }
}

}  // namespace mongo
