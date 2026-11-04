// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_options_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Entry point for a collMod that sets validationLevel to 'constraint'. First rejects requests that
 * collMod would refuse regardless of the data (the collection is not at 'strict', prepare has not
 * been set, the same request changes the validator or sets validationAction to 'warn'), so they do
 * not pay for a scan. Otherwise reads the collection validator from the local catalog, builds a
 * violation-scan aggregate, and executes it via CommandHelpers::runCommandDirectly. Returns a
 * non-OK status if any document violates the validator, or Status::OK() if all documents conform,
 * the collection has no validator, or it does not exist.
 *
 * 'placementConcern' is passed to the collection acquisition used to read the validator.
 * 'localOnly' controls whether the scan runs as a local 'aggregate' or fans out via
 * 'clusterAggregate'. 'newAction' and 'hasNewValidator' are the validationAction and whether a
 * validator is present in the same collMod request.
 */
[[MONGO_MOD_PUBLIC]] Status noDocumentsViolatingValidator(
    OperationContext* opCtx,
    const NamespaceString& nss,
    PlacementConcern placementConcern,
    bool localOnly,
    boost::optional<ValidationActionEnum> newAction,
    bool hasNewValidator);

}  // namespace mongo
