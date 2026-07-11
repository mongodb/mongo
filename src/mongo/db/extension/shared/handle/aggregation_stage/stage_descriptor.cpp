// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"

#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension {

AggStageParseNodeHandle AggStageDescriptorAPI::parse(BSONObj stageBson) const {
    ::MongoExtensionAggStageParseNode* parseNodePtr{nullptr};
    // The API's contract mandates that parseNodePtr will only be allocated if status is OK.
    invokeCAndConvertStatusToException(
        [&]() { return _vtable().parse(get(), objAsByteView(stageBson), &parseNodePtr); });
    return AggStageParseNodeHandle(parseNodePtr);
}
}  // namespace mongo::extension
