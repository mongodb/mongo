/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/change_stream_pre_image_helpers.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
void writeToChangeStreamPreImagesCollection(OperationContext* opCtx,
                                            const ChangeStreamPreImage& preImage) {
    const auto collectionNamespace = NamespaceString::kChangeStreamPreImagesNamespace;
    tassert(5869404,
            str::stream() << "Invalid pre-image document applyOpsIndex: "
                          << preImage.getId().getApplyOpsIndex(),
            preImage.getId().getApplyOpsIndex() >= 0);

    // This lock acquisition can block on a stronger lock held by another operation modifying the
    // pre-images collection. There are no known cases where an operation holding an exclusive lock
    // on the pre-images collection also waits for oplog visibility.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    AutoGetCollection preimagesCollectionRaii(opCtx, collectionNamespace, LockMode::MODE_IX);
    UpdateResult res = Helpers::upsert(opCtx, collectionNamespace.toString(), preImage.toBSON());
    tassert(5868601,
            str::stream() << "Failed to insert a new document into the pre-images collection: ts: "
                          << preImage.getId().getTs().toString()
                          << ", applyOpsIndex: " << preImage.getId().getApplyOpsIndex(),
            !res.existing && !res.upsertedId.isEmpty());
}
}  // namespace mongo
