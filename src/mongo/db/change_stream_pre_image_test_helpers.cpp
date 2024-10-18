/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_image_test_helpers.h"

#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/collection_crud/collection_write_path.h"

namespace mongo {
namespace change_stream_pre_image_test_helper {

void createPreImagesCollection(OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    ChangeStreamPreImagesCollectionManager::get(opCtx).createPreImagesCollection(opCtx, tenantId);
}

void insertDirectlyToPreImagesCollection(OperationContext* opCtx,
                                         boost::optional<TenantId> tenantId,
                                         const ChangeStreamPreImage& preImage) {
    const auto preImagesAcq = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::makePreImageCollectionNSS(tenantId),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    WriteUnitOfWork wuow(opCtx);
    uassertStatusOK(collection_internal::insertDocument(
        opCtx, preImagesAcq.getCollectionPtr(), InsertStatement{preImage.toBSON()}, nullptr));
    wuow.commit();
}

ChangeStreamPreImage makePreImage(const UUID& nsUUID,
                                  const Timestamp& timestamp,
                                  const Date_t& wallTime) {
    ChangeStreamPreImageId preImageId(nsUUID, timestamp, 0);
    return ChangeStreamPreImage{std::move(preImageId), wallTime, BSON("randomField" << 'a')};
}

CollectionTruncateMarkers::RecordIdAndWallTime extractRecordIdAndWallTime(
    const ChangeStreamPreImage& preImage) {
    return {change_stream_pre_image_util::toRecordId(preImage.getId()),
            preImage.getOperationTime()};
}

BSONObj toBSON(const CollectionTruncateMarkers::Marker& preImageMarker) {
    BSONObjBuilder builder;
    builder.append("records", preImageMarker.records);
    builder.append("bytes", preImageMarker.bytes);
    preImageMarker.lastRecord.serializeToken("lastRecord", &builder);
    if (!preImageMarker.lastRecord.isNull()) {
        builder.append(
            "lastRecordTimestamp",
            change_stream_pre_image_util::getPreImageTimestamp(preImageMarker.lastRecord));
    }
    builder.append("wallTime", preImageMarker.wallTime);

    return builder.obj();
}

int64_t bytes(const ChangeStreamPreImage& preImage) {
    return preImage.toBSON().objsize();
}

CollectionAcquisition acquirePreImagesCollectionForRead(OperationContext* opCtx,
                                                        boost::optional<TenantId> tenantId) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::makePreImageCollectionNSS(tenantId),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

}  // namespace change_stream_pre_image_test_helper
}  // namespace mongo
