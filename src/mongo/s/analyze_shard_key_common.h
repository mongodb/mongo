// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace analyze_shard_key {

// The maximum number of decimal places for the metrics returned by the analyzeShardKey command.
const int kMaxNumDecimalPlaces = 10;

/**
 * If the namespace corresponds to an internal collection, returns an IllegalOperation error.
 * Otherwise, returns an OK status.
 */
Status validateNamespace(const NamespaceString& nss);

/**
 * If the operation has a readConcern, returns a BSON object of the following form:
 * { level: "...",
 *   afterClusterTime: Timestamp(...) }
 *
 * Otherwise, returns an empty BSON object.
 */
repl::ReadConcernArgs extractReadConcern(OperationContext* opCtx);

/**
 * If the shard key is invalid, returns a BadValue error. Otherwise, returns an OK status. This
 * helper needs to be defined inline to avoid circular dependency with the IDL files.
 */
inline Status validateShardKeyPattern(const KeyPattern& shardKey) {
    ShardKeyPattern shardKeyPattern(shardKey);
    return Status::OK();
}

/**
 * Rounds 'val' to 'n' decimal places.
 */
double round(double val, int n);

/**
 * Returns the percentage between 'part' and 'whole' (between 0 and 100).
 */
double calculatePercentage(double part, double whole);

}  // namespace analyze_shard_key
}  // namespace mongo
