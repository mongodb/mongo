/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <functional>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"

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
Status repairDatabase(OperationContext* opCtx, StorageEngine* engine, const std::string& dbName);

/**
 * Repairs a collection using a storage engine-specific, best-effort process.
 * Some data may be lost or modified in the process but the result will be a readable collection
 * consistent with its indexes on a successful return.
 */
Status repairCollection(OperationContext* opCtx, StorageEngine* engine, const NamespaceString& nss);

}  // namespace repair
}  // namespace mongo
