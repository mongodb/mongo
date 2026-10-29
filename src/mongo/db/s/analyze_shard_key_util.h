// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace analyze_shard_key {

// The size limit for the documents to insert in a single command. The 2MB padding is to account
// for the size of the fields other than the "documents" field, and the fact that BSON stores an
// array as {'0': <object>, '1': <object>, ...}. The math is as follows. The limit for the number
// of documents that can be included in a single insert command is 100'000. So the size of the
// largest field name is 5 bytes (since the max index is 99999). There is 1 byte doc for the field
// name's null terminator and 1 byte per document for the type. So the upper bound for the overhead
// for the "documents" field is 700kB. The remaining > 1MB should be more than enough for the other
// fields in the insert command.
constexpr int kMaxBSONObjSizePerInsertBatch = BSONObjMaxUserSize - 2 * 1024 * 1024;

//
// The helpers used for validation within the analyzeShardKey or configureQueryAnalyzer command.

/*
 * If the namespace doesn't exist locally, returns a NamespaceNotFound error. If the collection is a
 * timeseries collection or has queryable encryption enabled, returns an IllegalOperation error. If
 * the namespace corresponds to a view, returns a CommandNotSupportedOnView error. Throws
 * DBException on any error that occurs during the validation. If the validation passed, returns an
 * OK status and the collection UUID for the collection when the validation occurred.
 */
StatusWith<UUID> validateCollectionOptions(OperationContext* opCtx, const NamespaceString& nss);

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
 * Returns the collection uuid for the collection if it exists.
 */
boost::optional<UUID> getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss);

//
// Other helpers.

/*
 * Returns true if the client is internal.
 */
bool isInternalClient(OperationContext* opCtx);

}  // namespace analyze_shard_key
}  // namespace mongo
