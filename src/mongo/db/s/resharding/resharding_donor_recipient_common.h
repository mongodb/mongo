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
#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace resharding {

using ReshardingFields = TypeCollectionReshardingFields;

/**
 * Looks up the StateMachine by the 'reshardingUUID'. Returns boost::none in the following cases:
 * 1. The state machine does not exist.
 * 2. In certain cases when the node is shutting down.
 * Additionally returns a bool indicating if the node is stepping or shutting down to disambiguate
 * the two.
 */
template <class Service, class StateMachine, class ReshardingDocument>
std::pair<boost::optional<std::shared_ptr<StateMachine>>, bool>
tryGetReshardingStateMachineAndShutdownState(OperationContext* opCtx, const UUID& reshardingUUID) {
    auto instanceId = BSON(ReshardingDocument::kReshardingUUIDFieldName << reshardingUUID);
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(Service::kServiceName);
    return StateMachine::lookup(opCtx, service, instanceId);
}

/**
 * Same as tryGetReshardingStateMachineAndShutdownState, except does not return the shutdown state.
 */
template <class Service, class StateMachine, class ReshardingDocument>
boost::optional<std::shared_ptr<StateMachine>> tryGetReshardingStateMachine(
    OperationContext* opCtx, const UUID& reshardingUUID) {
    auto [instance, _] =
        tryGetReshardingStateMachineAndShutdownState<Service, StateMachine, ReshardingDocument>(
            opCtx, reshardingUUID);
    return instance;
}

/**
 * Same as tryGetReshardingStateMachine, except throws if we were stepping or shutting down when we
 * tried to access the PrimaryOnlyService. Use this function in situations where you need to
 * guarantee that a return of boost::none means that there is no state document on disk for the
 * associated state machine.
 */
template <class Service, class StateMachine, class ReshardingDocument>
boost::optional<std::shared_ptr<StateMachine>> tryGetReshardingStateMachineAndThrowIfShuttingDown(
    OperationContext* opCtx, const UUID& reshardingUUID) {
    auto [instance, steppingOrShuttingDown] =
        tryGetReshardingStateMachineAndShutdownState<Service, StateMachine, ReshardingDocument>(
            opCtx, reshardingUUID);

    uassert(ErrorCodes::InterruptedDueToReplStateChange,
            "Unable to get resharding state machine, if it exists, because the node is "
            "stepping or shutting down.",
            !steppingOrShuttingDown);

    return instance;
}

/**
 * The following functions construct a ReshardingDocument from the given 'reshardingFields'.
 */
ReshardingDonorDocument constructDonorDocumentFromReshardingFields(
    const VersionContext& vCtx,
    const NamespaceString& nss,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields);

ReshardingRecipientDocument constructRecipientDocumentFromReshardingFields(
    const VersionContext& vCtx,
    const NamespaceString& nss,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields);

/**
 * Takes in the reshardingFields from a collection's config.collections entry and gives the
 * corresponding ReshardingDonorStateMachine or ReshardingRecipientStateMachine the updated
 * information. Will construct a ReshardingDonorStateMachine or ReshardingRecipientStateMachine if:
 *     1. The reshardingFields state indicates that the resharding operation is new, and
 *     2. A state machine does not exist on this node for the given namespace.
 */
void processReshardingFieldsForCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const CollectionMetadata& metadata,
                                          const ReshardingFields& reshardingFields);

void clearFilteringMetadata(OperationContext* opCtx, bool scheduleAsyncRefresh);

void clearFilteringMetadata(OperationContext* opCtx,
                            stdx::unordered_set<NamespaceString> namespacesToRefresh,
                            bool scheduleAsyncRefresh);

void refreshShardVersion(OperationContext* opCtx, const NamespaceString& nss);

}  // namespace resharding
}  // namespace mongo
