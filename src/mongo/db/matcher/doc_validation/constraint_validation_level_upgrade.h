// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo {

/**
 * Callback type for executing the violation-scan aggregate. Receives the pre-built request and
 * the required privilege vector; returns the raw cursor-response BSONObj or a non-OK status.
 * The shared impl handles everything else (collection read, pipeline, privilege construction,
 * cursor parsing, and error formatting).
 */
using ValidatorScanFn =
    std::function<StatusWith<BSONObj>(AggregateCommandRequest&, const PrivilegeVector&)>;

/**
 * Returns a ValidatorScanFn that runs the aggregate locally via runAggregate.
 * Use on standalone nodes and shard-local collMod execution.
 */
[[MONGO_MOD_PUBLIC]] ValidatorScanFn makeLocalValidatorScanFn(OperationContext* opCtx);

/**
 * Returns a ValidatorScanFn that runs the aggregate via ClusterAggregate, fanning out to all
 * shards. Use on the DDL coordinator for sharded collections.
 */
[[MONGO_MOD_PUBLIC]] ValidatorScanFn makeClusterValidatorScanFn(OperationContext* opCtx);

/**
 * Reads the collection validator from the local catalog, builds a violation-scan aggregate, and
 * executes it via 'runAgg'. Returns a non-OK status if any document violates the validator, or
 * Status::OK() if all documents conform or the collection has no validator.
 *
 * 'placementConcern' is passed to the collection acquisition used to read the validator.
 * 'runAgg' is responsible only for dispatching the aggregate and returning the raw response;
 * the caller supplies the appropriate executor (local runAggregate or ClusterAggregate).
 */
[[MONGO_MOD_PUBLIC]] Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                                          const NamespaceString& nss,
                                                          PlacementConcern placementConcern,
                                                          ValidatorScanFn runAgg);

}  // namespace mongo
