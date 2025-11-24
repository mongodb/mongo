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
