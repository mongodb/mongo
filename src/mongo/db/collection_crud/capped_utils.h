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

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/util/uuid.h"

namespace mongo {
class Database;
class NamespaceString;
class OperationContext;

/**
 * Clones the collection "shortFrom" to the capped collection "shortTo" with a size of "size".
 * If targetUUID is provided, then the newly capped collection will get that UUID. Otherwise, the
 * UUID for the newly capped collection will be randomly generated.
 */
void cloneCollectionAsCapped(OperationContext* opCtx,
                             Database* db,
                             const NamespaceString& fromNss,
                             const NamespaceString& toNss,
                             long long size,
                             bool temp,
                             const boost::optional<UUID>& targetUUID = boost::none);

/**
 * Converts the collection "collectionName" to a capped collection with a size of "size".
 * If targetUUID is provided, then the newly capped collection will get that UUID. Otherwise, the
 * UUID for the newly capped collection will be randomly generated.
 */
void convertToCapped(OperationContext* opCtx,
                     const NamespaceString& collectionName,
                     long long size,
                     const boost::optional<UUID>& targetUUID = boost::none);
}  // namespace mongo
