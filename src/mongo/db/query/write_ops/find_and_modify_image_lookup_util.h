// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/modules.h"

namespace mongo {

using FindOneLocallyFunc = std::function<boost::optional<BSONObj>(
    const NamespaceString& nss,
    const BSONObj& filter,
    const boost::optional<repl::ReadConcernArgs>& readConcern)>;

/**
 * Fetches and returns the pre- or post-image of the findAndModify operation by performing a
 * snapshot read against the collection wrote to. If the operation was executed in a transaction,
 * the oplog entry must have the commit timestamp.
 */
[[MONGO_MOD_PUBLIC]] boost::optional<BSONObj> fetchPreOrPostImageFromSnapshot(
    const repl::OplogEntry& oplogEntry, FindOneLocallyFunc findOneLocallyFunc);

/**
 * Fetches the pre- or post-image for the given findAndModify operation either from the image
 * collection or by performing a snapshot read against the collection the findAndModify wrote to.
 * Returns a forged noop oplog entry containing the image. Returns none if no image is found.
 */
[[MONGO_MOD_PUBLIC]] boost::optional<repl::OplogEntry> forgeNoopImageOplogEntry(
    OperationContext* opCtx,
    const repl::OplogEntry& oplogEntry,
    FindOneLocallyFunc findOneLocallyFunc);

}  // namespace mongo
