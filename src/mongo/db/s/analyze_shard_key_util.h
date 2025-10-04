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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
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
