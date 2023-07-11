/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"

#include <fmt/format.h>

#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/repl/tenant_migration_decoration.h"

namespace mongo {

namespace {

/**
 * Inserts the document into the pre-images collection. The document is inserted into the
 * tenant's pre-images collection if the 'tenantId' is specified.
 */
void writeChangeStreamPreImageEntry(
    OperationContext* opCtx,
    // Skip the pre-image insert if we are in the middle of a tenant migration. Pre-image inserts
    // for writes during the oplog catchup phase are handled in the oplog application code.
    boost::optional<TenantId> tenantId,
    const ChangeStreamPreImage& preImage) {
    if (repl::tenantMigrationInfo(opCtx)) {
        return;
    }

    ChangeStreamPreImagesCollectionManager::get(opCtx).insertPreImage(opCtx, tenantId, preImage);
}

}  // namespace

void ChangeStreamPreImagesOpObserver::onUpdate(OperationContext* opCtx,
                                               const OplogUpdateEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
    if (!opAccumulator) {
        return;
    }

    // Write a pre-image to the change streams pre-images collection when following conditions
    // are met:
    // 1. The collection has 'changeStreamPreAndPostImages' enabled.
    // 2. The node wrote the oplog entry for the corresponding operation.
    // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
    //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
    //    change streams and storing them in pre-image collection is redundant.
    // 4. a request to update is not on a temporary resharding collection. This update request
    //    does not result in change streams events. Recording pre-images from temporary
    //    resharing collection could result in incorrect pre-image getting recorded due to the
    //    temporary resharding collection not being consistent until writes are blocked (initial
    //    sync mode application).
    const auto& opTimeBundle = opAccumulator->opTime;
    const auto& nss = args.coll->ns();
    if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection &&
        !opTimeBundle.writeOpTime.isNull() &&
        args.updateArgs->source != OperationSource::kFromMigrate &&
        !nss.isTemporaryReshardingCollection()) {
        const auto& preImageDoc = args.updateArgs->preImageDoc;
        const auto uuid = args.coll->uuid();
        invariant(!preImageDoc.isEmpty(),
                  fmt::format("PreImage must be set when writing to change streams pre-images "
                              "collection for update on collection {} (UUID: {}) with optime {}",
                              nss.toStringForErrorMsg(),
                              uuid.toString(),
                              opTimeBundle.writeOpTime.toString()));

        ChangeStreamPreImageId id(uuid, opTimeBundle.writeOpTime.getTimestamp(), 0);
        ChangeStreamPreImage preImage(id, opTimeBundle.wallClockTime, preImageDoc);

        writeChangeStreamPreImageEntry(opCtx, args.coll->ns().tenantId(), preImage);
    }
}

void ChangeStreamPreImagesOpObserver::onDelete(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               StmtId stmtId,
                                               const OplogDeleteEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
    if (!opAccumulator) {
        return;
    }

    // Write a pre-image to the change streams pre-images collection when following conditions
    // are met:
    // 1. The collection has 'changeStreamPreAndPostImages' enabled.
    // 2. The node wrote the oplog entry for the corresponding operation.
    // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
    //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
    //    change streams and storing them in pre-image collection is redundant.
    // 4. a request to delete is not on a temporary resharding collection. This delete request
    //    does not result in change streams events. Recording pre-images from temporary
    //    resharing collection could result in incorrect pre-image getting recorded due to the
    //    temporary resharding collection not being consistent until writes are blocked (initial
    //    sync mode application).
    const auto& opTimeBundle = opAccumulator->opTime;
    const auto& nss = coll->ns();
    if (args.changeStreamPreAndPostImagesEnabledForCollection &&
        !opTimeBundle.writeOpTime.isNull() && !args.fromMigrate &&
        !nss.isTemporaryReshardingCollection()) {
        const auto& preImageDoc = args.deletedDoc;
        const auto uuid = coll->uuid();
        invariant(
            !preImageDoc->isEmpty(),
            fmt::format("Deleted document must be set when writing to change streams pre-images "
                        "collection for update on collection {} (UUID: {}) with optime {}",
                        nss.toStringForErrorMsg(),
                        uuid.toString(),
                        opTimeBundle.writeOpTime.toString()));

        ChangeStreamPreImageId id(uuid, opTimeBundle.writeOpTime.getTimestamp(), 0);
        ChangeStreamPreImage preImage(id, opTimeBundle.wallClockTime, *preImageDoc);

        writeChangeStreamPreImageEntry(opCtx, nss.tenantId(), preImage);
    }
}

}  // namespace mongo
