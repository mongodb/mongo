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
MONGO_MOD_PUBLIC ValidatorScanFn makeLocalValidatorScanFn(OperationContext* opCtx);

/**
 * Returns a ValidatorScanFn that runs the aggregate via ClusterAggregate, fanning out to all
 * shards. Use on the DDL coordinator for sharded collections.
 */
MONGO_MOD_PUBLIC ValidatorScanFn makeClusterValidatorScanFn(OperationContext* opCtx);

/**
 * Reads the collection validator from the local catalog, builds a violation-scan aggregate, and
 * executes it via 'runAgg'. Returns a non-OK status if any document violates the validator, or
 * Status::OK() if all documents conform or the collection has no validator.
 *
 * 'placementConcern' is passed to the collection acquisition used to read the validator.
 * 'runAgg' is responsible only for dispatching the aggregate and returning the raw response;
 * the caller supplies the appropriate executor (local runAggregate or ClusterAggregate).
 */
MONGO_MOD_PUBLIC Status noDocumentsViolatingValidator(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      PlacementConcern placementConcern,
                                                      ValidatorScanFn runAgg);

}  // namespace mongo
