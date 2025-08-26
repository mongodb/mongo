/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class Collection;
class CollectionPtr;
class NamespaceString;
class OperationContext;

/**
 * Adds UUIDs to all replicated collections of all databases if they do not already have UUIDs. If
 * this function is not necessary for SERVER-33247, it can be removed.
 */
void addCollectionUUIDs(OperationContext* opCtx);

/**
 * Checks if the collMod request is converting an index to unique.
 */
bool isCollModIndexUniqueConversion(const CollModRequest& request);

/**
 * Constructs a valid collMod dry-run request from the original request.
 * The 'dryRun' option can only be used with the index 'unique' option, so we assume 'request' must
 * have the 'unique' option. The function will also remove other options from the original request.
 */
CollModRequest makeCollModDryRunRequest(const CollModRequest& request);

/**
 * Performs the collection modification described in "cmd" on the collection "ns". The 'acquisition'
 * parameter is intended to be used for internal collMod commands where the collection has already
 * been acquired with the necessary X lock. If omitted, the collection will be looked up and locked
 * appropriately.
 */
Status processCollModCommand(OperationContext* opCtx,
                             const NamespaceStringOrUUID& nsOrUUID,
                             const CollMod& cmd,
                             CollectionAcquisition* acquisition,
                             BSONObjBuilder* result);

/**
 * Performs static validation of CollMod request.
 *
 * Static checks are the ones perfomed exclusively on the request itself without accessing the
 * catalog.
 */
void staticValidateCollMod(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const CollModRequest& request);

/**
 * Performs the collection modification described in "cmd" on the collection "ns". Only checks for
 * duplicates for the 'applyOps' command.
 */
Status processCollModCommandForApplyOps(OperationContext* opCtx,
                                        const NamespaceStringOrUUID& nsOrUUID,
                                        const CollMod& cmd,
                                        repl::OplogApplication::Mode mode);

}  // namespace mongo
