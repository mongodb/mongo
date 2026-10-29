// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/index_builds/rebuild_indexes.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_key_validate.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

Status rebuildIndexesOnCollection(OperationContext* opCtx, CollectionWriter& collWriter) {
    std::vector<std::string> indexNames;
    collWriter->getAllIndexes(&indexNames);
    if (indexNames.empty())
        return Status::OK();

    std::vector<BSONObj> indexSpecs;
    indexSpecs.reserve(indexNames.size());

    for (const auto& name : indexNames) {
        BSONObj spec = collWriter->getIndexSpec(name);
        using IndexVersion = IndexDescriptor::IndexVersion;
        IndexVersion indexVersion = IndexVersion::kV1;
        if (auto indexVersionElem = spec[IndexDescriptor::kIndexVersionFieldName]) {
            auto indexVersionNum = indexVersionElem.numberInt();
            invariant(indexVersionNum == static_cast<int>(IndexVersion::kV1) ||
                      indexVersionNum == static_cast<int>(IndexVersion::kV2));
            indexVersion = static_cast<IndexVersion>(indexVersionNum);
        }
        invariant(spec.isOwned());
        indexSpecs.push_back(spec);

        const BSONObj key = spec.getObjectField("key");
        const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
        if (!keyStatus.isOK()) {
            return Status(
                ErrorCodes::CannotCreateIndex,
                str::stream()
                    << "Cannot rebuild index " << spec << ": " << keyStatus.reason()
                    << " For more info see http://dochub.mongodb.org/core/index-validation");
        }
    }

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    UUID buildUUID = UUID::gen();
    auto swRebuild =
        indexBuildsCoord->rebuildIndexesForRecovery(opCtx, collWriter, indexSpecs, buildUUID);
    if (!swRebuild.isOK()) {
        return swRebuild.getStatus();
    }

    auto [numRecords, dataSize] = swRebuild.getValue();

    // Update the record store stats after finishing and committing the index builds.
    collWriter->getRecordStore()->updateStatsAfterRepair(numRecords, dataSize);

    return Status::OK();
}

}  // namespace mongo
