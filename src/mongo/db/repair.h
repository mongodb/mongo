// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/record_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>

namespace mongo {
class StorageEngine;
class NamespaceString;
class OperationContext;
class Status;

namespace repair {

/**
 * Repairs a database using a storage engine-specific, best-effort process. Some data may be lost or
 * modified in the process but the result will be readable collections consistent with their indexes
 * on a successful return.
 *
 * It is expected that the local database will be repaired first when running in repair mode.
 */
Status repairDatabase(OperationContext* opCtx, StorageEngine* engine, const DatabaseName& dbName);

/**
 * Repairs a collection using a storage engine-specific, best-effort process.
 * Some data may be lost or modified in the process but the result will be a readable collection
 * consistent with its indexes on a successful return.
 */
Status repairCollection(OperationContext* opCtx, StorageEngine* engine, const NamespaceString& nss);

}  // namespace repair
}  // namespace mongo
