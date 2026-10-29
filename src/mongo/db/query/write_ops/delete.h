// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class Database;
class OperationContext;
class CollectionAcquisition;

/**
 * Deletes objects from 'collection' that match the query predicate given by 'pattern'. If 'justOne'
 * is true, deletes only the first matching object. The PlanExecutor used to do the deletion will
 * not yield. If 'god' is true, deletes are allowed on system namespaces.
 */
long long deleteObjects(OperationContext* opCtx,
                        const CollectionAcquisition& collection,
                        BSONObj pattern,
                        bool justOne,
                        bool god = false,
                        bool fromMigrate = false);

struct DeleteResult {
    long long nDeleted;
    boost::optional<BSONObj> requestedPreImage;
};

DeleteResult deleteObject(OperationContext* opCtx,
                          const CollectionAcquisition& collection,
                          const DeleteRequest& request);

}  // namespace mongo
