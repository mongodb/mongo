// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/vector_clock/vector_clock_metadata_hook.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_gen.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {

namespace rpc {

namespace {
const char kOperationTimeFieldName[] = "operationTime";
}

VectorClockMetadataHook::VectorClockMetadataHook(ServiceContext* service) : _service(service) {}

Status VectorClockMetadataHook::writeRequestMetadata(OperationContext* opCtx,
                                                     BSONObjBuilder* metadataBob) {
    VectorClock::get(_service)->gossipOut(opCtx, metadataBob, true /* forceInternal */);
    return Status::OK();
}

Status VectorClockMetadataHook::readReplyMetadata(OperationContext* opCtx,
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
                    operationTime.type() == BSONType::timestamp);
            timeTracker->updateOperationTime(LogicalTime(operationTime.timestamp()));
        }
    }

    auto receivedComponents = GossipedVectorClockComponents::parse(
        metadataObj, IDLParserContext("VectorClockComponents"));
    VectorClock::get(_service)->gossipIn(opCtx,
                                         receivedComponents,
                                         false /* couldBeUnauthorized */,
                                         true /* defaultIsInternalClient */);
    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
