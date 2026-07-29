// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Reads the collection validator from the local catalog, builds a violation-scan aggregate, and
 * executes it via CommandHelpers::runCommandDirectly. Returns a non-OK status if any document
 * violates the validator, or Status::OK() if all documents conform or the collection has no
 * validator.
 *
 * 'placementConcern' is passed to the collection acquisition used to read the validator.
 * 'localOnly' controls whether the scan runs as a local 'aggregate' or fans out via
 * 'clusterAggregate'.
 */
[[MONGO_MOD_PUBLIC]] Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          PlacementConcern placementConcern,
                                                          bool localOnly);

}  // namespace mongo
