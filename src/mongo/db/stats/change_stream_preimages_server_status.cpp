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

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/transaction_resources.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {
namespace {

void appendPreImagesCollectionStats(OperationContext* opCtx, BSONObjBuilder* result) {
    const auto preImagesCollectionNamespace =
        NamespaceString::makePreImageCollectionNSS(boost::none /** tenantId */);
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto preImagesColl = catalog->lookupCollectionByNamespace(opCtx, preImagesCollectionNamespace);
    if (!preImagesColl) {
        return;
    }

    const auto expireAfterSeconds = change_stream_pre_image_util::getExpireAfterSeconds(opCtx);
    if (expireAfterSeconds) {
        result->append("expireAfterSeconds", expireAfterSeconds->count());
    }

    int64_t numRecords = preImagesColl->numRecords(opCtx);
    int64_t dataSize = preImagesColl->dataSize(opCtx);
    result->append("numDocs", numRecords);
    result->append("totalBytes", dataSize);

    if (numRecords) {
        result->append("avgDocSize", preImagesColl->averageObjectSize(opCtx));
    }

    result->append("docsInserted",
                   ChangeStreamPreImagesCollectionManager::get(opCtx).getDocsInserted());


    // Obtaining 'storageSize' and 'freeStorageSize' requires obtaining the GlobalLock in MODE_IS.
    //
    // Fields are omitted if it cannot be immediately acquired to prevent stalling 'serverStatus'
    // observability.
    ScopedAdmissionPriority skipAdmissionControl(opCtx, AdmissionContext::Priority::kExempt);
    Lock::GlobalLock lk(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kLeaveUnlocked, [] {
        Lock::GlobalLockSkipOptions options;
        options.skipRSTLLock = true;
        return options;
    }());
    if (!lk.isLocked()) {
        LOGV2_DEBUG(7790900,
                    2,
                    "Failed to get global lock for 'changeStreamPreImages' serverStatus "
                    "'storageSize' and 'freeStorageSize' fields");
        return;
    }

    result->append("storageSize", preImagesColl->getRecordStore()->storageSize(opCtx));
    result->append("freeStorageSize", preImagesColl->getRecordStore()->freeStorageSize(opCtx));
}
}  // namespace

/**
 * Adds a section 'changeStreamPreImages' to the serverStatus metrics that provides aggregated
 * statistics for change stream pre-images.
 */
class ChangeStreamPreImagesServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;

        // Purging metrics are reported regardless of whether it is a single or multi-tenant
        // environment. In the case of a multi tenant environment, the metrics are cumulative across
        // tenants.
        const auto& jobStats =
            ChangeStreamPreImagesCollectionManager::get(opCtx).getPurgingJobStats();
        builder.append("purgingJob", jobStats.toBSON());

        if (!change_stream_serverless_helpers::isChangeCollectionsModeActive()) {
            // Only report pre-images collection specific metrics in single tenant environments.
            // Multi-tenant environments would have the aggregate result of all the tenants (one
            // pre-images collection per tenant), making it difficult to pinpoint where the metrics
            // values come from.
            appendPreImagesCollectionStats(opCtx, &builder);
        }

        return builder.obj();
    }
};
auto& changeStreamPreImagesServerStatus =
    *ServerStatusSectionBuilder<ChangeStreamPreImagesServerStatus>("changeStreamPreImages")
         .forShard();

}  // namespace mongo
