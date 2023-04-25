/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
namespace analyze_shard_key {

// The maximum number of decimal places for the metrics returned by the analyzeShardKey command.
const int kMaxNumDecimalPlaces = 10;

// The size limit for the documents to an insert in a single batch. Leave some padding for other
// fields in the insert command.
constexpr int kMaxBSONObjSizePerInsertBatch = BSONObjMaxUserSize - 100 * 1024;

//
// The helpers used for validation within the analyzeShardKey or configureQueryAnalyzer command.

/*
 * If the namespace corresponds to an internal collection, returns an IllegalOperation error.
 * Otherwise, returns an OK status.
 */
Status validateNamespace(const NamespaceString& nss);

/*
 * If the namespace doesn't exist locally, returns a NamespaceNotFound error. If the namespace
 * corresponds to a view, returns a CommandNotSupportedOnView error. If the collection has
 * queryable encryption enabled, returns an IllegalOperation error. Throws DBException on any error
 * that occurs during the validation. If the validation passed, returns an OK status and the
 * collection UUID for the collection when the validation occurred.
 */
StatusWith<UUID> validateCollectionOptions(OperationContext* opCtx, const NamespaceString& nss);

/*
 * If the shard key is invalid, returns a BadValue error. Otherwise, returns an OK status. This
 * helper needs to be defined inline to avoid circular dependency with the IDL files.
 */
inline Status validateShardKeyPattern(const KeyPattern& shardKey) {
    ShardKeyPattern shardKeyPattern(shardKey);
    return Status::OK();
}

/*
 * If the index key cannot be used as a shard key index, returns a BadValue error. Otherwise,
 * returns an OK status.
 */
Status validateIndexKey(const BSONObj& indexKey);

/**
 * If the given shard key value contains an array field, throws a BadValue error.
 */
void uassertShardKeyValueNotContainArrays(const BSONObj& value);

/**
 * If the operation has a readConcern, returns a BSON object of the following form:
 * { level: "...",
 *   afterClusterTime: Timestamp(...) }
 *
 * Otherwise, returns an empty BSON object.
 */
BSONObj extractReadConcern(OperationContext* opCtx);

//
// Other helpers.

/*
 * Rounds 'val' to 'n' decimal places.
 */
double round(double val, int n);

/*
 * Returns the percentage between 'part' and 'whole' (between 0 and 100).
 */
double calculatePercentage(double part, double whole);

}  // namespace analyze_shard_key
}  // namespace mongo
