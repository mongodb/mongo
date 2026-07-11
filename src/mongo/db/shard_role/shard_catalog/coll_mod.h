// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Checks if the collMod request is converting an index to unique.
 */
[[MONGO_MOD_PARENT_PRIVATE]]
bool isCollModIndexUniqueConversion(const CollModRequest& request);

/**
 * Constructs a valid collMod dry-run request from the original request.
 * The 'dryRun' option can only be used with the index 'unique' option, so we assume 'request' must
 * have the 'unique' option. The function will also remove other options from the original request.
 */
[[MONGO_MOD_PARENT_PRIVATE]]
CollModRequest makeCollModDryRunRequest(const CollModRequest& request);

/**
 * Performs the collection modification described in "cmd" on the collection "ns". The 'acquisition'
 * parameter is intended to be used for internal collMod commands where the collection has already
 * been acquired with the necessary X lock. If omitted, the collection will be looked up and locked
 * appropriately.
 */
[[MONGO_MOD_PUBLIC]]
Status processCollModCommand(OperationContext* opCtx,
                             const NamespaceStringOrUUID& nsOrUUID,
                             const CollMod& cmd,
                             CollectionAcquisition* acquisition,
                             BSONObjBuilder* result);

/**
 * Returns true if the given collmod @request contains options related to timeseries collections
 */
[[MONGO_MOD_PARENT_PRIVATE]]
bool hasTimeseriesOptions(const CollModRequest& request);

/**
 * Performs static validation of CollMod request.
 *
 * Static checks are the ones perfomed exclusively on the request itself without accessing the
 * catalog.
 */
[[MONGO_MOD_PARENT_PRIVATE]]
void staticValidateCollMod(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const CollModRequest& request);

/**
 * Performs the collection modification described in "cmd" on the collection "ns". Only checks for
 * duplicates for the 'applyOps' command.
 */
[[MONGO_MOD_PUBLIC]]
Status processCollModCommandForApplyOps(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const CollMod& cmd,
                                        repl::OplogApplication::Mode mode);

}  // namespace mongo
