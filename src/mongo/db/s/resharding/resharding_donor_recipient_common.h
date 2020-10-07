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

#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/s/resharding_util.h"

namespace mongo {
namespace resharding {

using ReshardingFields = TypeCollectionReshardingFields;

/**
 * Looks up the StateMachine by the 'reshardingUUID'. If it does not exist, returns boost::none.
 */
template <class Service, class StateMachine, class ReshardingDocument>
boost::optional<std::shared_ptr<StateMachine>> tryGetReshardingStateMachine(
    OperationContext* opCtx, const UUID& reshardingUUID) {
    auto instanceId = BSON(ReshardingDocument::k_idFieldName << reshardingUUID);
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(Service::kServiceName);
    return StateMachine::lookup(opCtx, service, instanceId);
}

/**
 * The following functions construct a ReshardingDocument from the given 'reshardingFields'.
 */
ReshardingDonorDocument constructDonorDocumentFromReshardingFields(
    const NamespaceString& nss,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields);

ReshardingRecipientDocument constructRecipientDocumentFromReshardingFields(
    OperationContext* opCtx,
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

}  // namespace resharding

}  // namespace mongo