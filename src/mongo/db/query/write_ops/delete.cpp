// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/delete.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/write_ops/canonical_delete.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/none.hpp>


namespace mongo {

long long deleteObjects(OperationContext* opCtx,
                        const CollectionAcquisition& collection,
                        BSONObj pattern,
                        bool justOne,
                        bool god,
                        bool fromMigrate) {
    auto request = DeleteRequest{};
    request.setNsString(collection.nss());
    request.setQuery(pattern);
    request.setMulti(!justOne);
    request.setGod(god);
    request.setFromMigrate(fromMigrate);
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

    auto canonicalDelete = uassertStatusOK(
        CanonicalDelete::makeFromRequest(opCtx, collection.getCollectionPtr(), request));

    auto exec = uassertStatusOK(getExecutorDelete(
        &CurOp::get(opCtx)->debug(), collection, canonicalDelete, boost::none /* verbosity */));

    return exec->executeDelete();
}

DeleteResult deleteObject(OperationContext* opCtx,
                          const CollectionAcquisition& collection,
                          const DeleteRequest& request) {
    auto canonicalDelete = uassertStatusOK(
        CanonicalDelete::makeFromRequest(opCtx, collection.getCollectionPtr(), request));

    auto exec = uassertStatusOK(getExecutorDelete(
        &CurOp::get(opCtx)->debug(), collection, canonicalDelete, boost::none /* verbosity */));

    if (!request.getReturnDeleted()) {
        return {exec->executeDelete(), boost::none};
    }

    // This method doesn't support multi-deletes when returning pre-images.
    tassert(11052000, "Expected single delete", !request.getMulti());

    BSONObj image;
    if (exec->getNext(&image, nullptr) == PlanExecutor::IS_EOF) {
        return {};
    }

    return {1, image.getOwned()};
}

}  // namespace mongo
