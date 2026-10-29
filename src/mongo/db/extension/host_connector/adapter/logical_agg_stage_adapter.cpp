// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host_connector/adapter/logical_agg_stage_adapter.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo::extension::host_connector {

::MongoExtensionByteView HostLogicalAggStageAdapter::_hostGetName(
    const ::MongoExtensionLogicalAggStage* logicalStage) noexcept {
    auto sv = static_cast<const HostLogicalAggStageAdapter*>(logicalStage)->getImpl().getName();
    std::string_view sd{sv.data(), sv.length()};
    return stringDataAsByteView(sd);
}

::MongoExtensionStatus* HostLogicalAggStageAdapter::_hostGetFilter(
    const ::MongoExtensionLogicalAggStage* logicalStage,
    ::MongoExtensionByteBuf** output) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        *output = nullptr;

        auto filter =
            static_cast<const HostLogicalAggStageAdapter*>(logicalStage)->getImpl().getFilter();

        // Null output indicates no filter - only initialize 'output' when we have a non-empty
        // filter.
        if (!filter.isEmpty()) {
            *output = new ByteBuf(filter);
        }
    });
}

}  // namespace mongo::extension::host_connector
