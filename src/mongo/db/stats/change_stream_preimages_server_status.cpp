// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/version_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kFTDC

namespace mongo {
namespace {

void appendPreImagesCollectionStats(OperationContext* opCtx, BSONObjBuilder* result) {
    const auto& preImagesCollectionNamespace = NamespaceString::kChangeStreamPreImagesNamespace;
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

    // Note: on disaggregated storage, the values returned for 'numRecords' and 'dataSize' may
    // always be 0.
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
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
        opCtx, AdmissionContext::Priority::kExempt);
    Lock::GlobalLock lk(opCtx, MODE_IS, Date_t::now(), Lock::InterruptBehavior::kLeaveUnlocked, [] {
        Lock::GlobalLockOptions options;
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

    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    result->append("storageSize", preImagesColl->getRecordStore()->storageSize(ru));
    result->append("freeStorageSize", preImagesColl->getRecordStore()->freeStorageSize(ru));
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

        const auto& jobStats =
            ChangeStreamPreImagesCollectionManager::get(opCtx).getPurgingJobStats();
        builder.append("purgingJob", jobStats.toBSON());

        // Report pre-images collection specific metrics.
        appendPreImagesCollectionStats(opCtx, &builder);

        const auto& markerCreationStats =
            ChangeStreamPreImagesCollectionManager::get(opCtx).getMarkerCreationStats();
        builder.append("markerCreation", markerCreationStats.toBSON());

        return builder.obj();
    }
};
auto& changeStreamPreImagesServerStatus =
    *ServerStatusSectionBuilder<ChangeStreamPreImagesServerStatus>("changeStreamPreImages")
         .forShard();

}  // namespace mongo
