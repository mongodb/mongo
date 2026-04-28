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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * Describes a single index to create: the key pattern and whether it is unique.
 */
struct IndexSpec_ForCatalog {
    BSONObj keys;
    bool unique;
};

/**
 * Returns the index specs that must exist on every chunks collection (both the
 * config-server's config.chunks and a shard's config.shard.catalog.chunks).
 * The caller supplies the target namespace.
 */
MONGO_MOD_PUBLIC std::vector<IndexSpec_ForCatalog> getChunkCollectionIndexSpecs();

/**
 * Ensures that the given namespace exists and has all of the indexes described
 * by `specs`.  Creates the collection if it doesn't yet exist.  Does not error
 * if an index already exists, so long as the options are the same.
 */
MONGO_MOD_PUBLIC Status ensureCollectionIndexes(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const std::vector<IndexSpec_ForCatalog>& specs);

/**
 * Builds an index on a config server collection.
 * Creates the collection if it doesn't yet exist.  Does not error if the index already exists,
 * so long as the options are the same.
 */
MONGO_MOD_NEEDS_REPLACEMENT Status createIndexOnConfigCollection(OperationContext* opCtx,
                                                                 const NamespaceString& ns,
                                                                 const BSONObj& keys,
                                                                 bool unique);

}  // namespace mongo
