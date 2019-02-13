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

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/free_mon/free_mon_storage_gen.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Storage tier for Free Monitoring. Provides access to storage engine.
 */
class FreeMonStorage {
public:
    /**
     * The _id value in admin.system.version.
     */
    static constexpr auto kFreeMonDocIdKey = "free_monitoring"_sd;

    /**
     * Reads document from disk if it exists.
     */
    static boost::optional<FreeMonStorageState> read(OperationContext* opCtx);

    /**
     * Replaces document on disk with contents of document. Creates document if it does not exist.
     */
    static void replace(OperationContext* opCtx, const FreeMonStorageState& doc);

    /**
     * Deletes document on disk if it exists.
     */
    static void deleteState(OperationContext* opCtx);

    /**
     * Reads the singelton document from local.clustermanager.
     *
     * Returns nothing if there are more then one document or it does not exist.
     */
    static boost::optional<BSONObj> readClusterManagerState(OperationContext* opCtx);
};

}  // namespace mongo
