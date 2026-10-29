// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * Generates a collection name suitable for creating a temporary collection.
 *
 * The name is based on a model that replaces each percent sign in 'collectionNameModel' by a random
 * character in the range [0-9A-Za-z].
 *
 * Throws FailedToParse if 'collectionNameModel' does not contain any percent signs.
 * Throws NamespaceExists if we are unable to generate a collection name that does not conflict with
 * an existing collection in this database.
 *
 * The database must be locked in MODE_IX when calling this function.
 */
[[MONGO_MOD_PRIVATE]] StatusWith<NamespaceString> makeUniqueCollectionName(
    OperationContext* opCtx, const DatabaseName& dbName, std::string_view collectionNameModel);

/**
 * Generates a random collection name suitable for creating a temporary collection. Does not check
 * for its existence in the catalog to avoid collisions.
 *
 * The name is based on a model that replaces each percent sign in 'collectionNameModel' by a random
 * character in the range [0-9A-Za-z].
 *
 * Throws FailedToParse if 'collectionNameModel' does not contain any percent signs.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] StatusWith<NamespaceString> generateRandomCollectionName(
    OperationContext* opCtx, const DatabaseName& dbName, std::string_view collectionNameModel);

}  // namespace mongo
