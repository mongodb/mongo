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

#include "mongo/s/shard_key_pattern.h"

namespace mongo {

/**
 * Given a simple BSON query, extracts the shard key corresponding to the key pattern from equality
 * matches in the query. The query expression *must not* be a complex query with sorts or other
 * attributes.
 *
 * Logically, the equalities in the BSON query can be serialized into a BSON document and then a
 * shard key is extracted from this equality document.
 *
 * NOTE: BSON queries and BSON documents look similar but are different languages. Use the correct
 * shard key extraction function.
 *
 * Returns !OK status if the query cannot be parsed.
 * Returns an empty BSONObj() if there is no shard key found in the query equalities.
 *
 * Examples:
 *  If the key pattern is { a : 1 }
 *   { a : "hi", b : 4 } --> returns { a : "hi" }
 *   { a : { $eq : "hi" }, b : 4 } --> returns { a : "hi" }
 *   { $and : [{a : { $eq : "hi" }}, { b : 4 }] } --> returns { a : "hi" }
 *  If the key pattern is { 'a.b' : 1 }
 *   { a : { b : "hi" } } --> returns { 'a.b' : "hi" }
 *   { 'a.b' : "hi" } --> returns { 'a.b' : "hi" }
 *   { a : { b : { $eq : "hi" } } } --> returns {} because the query language treats this as
 *                                                 a : { $eq : { b : ... } }
 */
StatusWith<BSONObj> extractShardKeyFromBasicQuery(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const ShardKeyPattern& shardKeyPattern,
                                                  const BSONObj& basicQuery);

/**
 * Variant of the above, which is used to parse queries that contain let parameters and runtime
 * constants.
 */
StatusWith<BSONObj> extractShardKeyFromBasicQueryWithContext(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const ShardKeyPattern& shardKeyPattern,
    const BSONObj& basicQuery);

}  // namespace mongo
