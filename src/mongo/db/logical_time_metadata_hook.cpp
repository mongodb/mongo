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

#include "mongo/platform/basic.h"

#include "mongo/db/logical_time_metadata_hook.h"

#include <memory>

#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/vector_clock.h"

namespace mongo {

namespace rpc {

namespace {
const char kOperationTimeFieldName[] = "operationTime";
}

// TODO SERVER-48434: Rename this class to VectorClockMetadataHook.
LogicalTimeMetadataHook::LogicalTimeMetadataHook(ServiceContext* service) : _service(service) {}

Status LogicalTimeMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                     BSONObjBuilder* metadataBob) {
    VectorClock::get(_service)->gossipOut(opCtx, metadataBob, transport::Session::kInternalClient);
    return Status::OK();
}

Status LogicalTimeMetadataHook::readReplyMetadata(OperationContext* opCtx,
                                                  StringData replySource,
                                                  const BSONObj& metadataObj) {
    if (!VectorClock::get(_service)->isEnabled()) {
        return Status::OK();
    }

    if (opCtx) {
        auto timeTracker = OperationTimeTracker::get(opCtx);
        auto operationTime = metadataObj[kOperationTimeFieldName];
        if (!operationTime.eoo()) {
            tassert(4457010,
                    "operationTime must be a timestamp if present",
                    operationTime.type() == BSONType::bsonTimestamp);
            timeTracker->updateOperationTime(LogicalTime(operationTime.timestamp()));
        }
    }

    VectorClock::get(_service)->gossipIn(
        opCtx, metadataObj, false /* couldBeUnauthorized */, transport::Session::kInternalClient);
    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
