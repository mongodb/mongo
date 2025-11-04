/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/aggregation_stage.h"

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

namespace mongo::extension::sdk {

MONGO_FAIL_POINT_DEFINE(failVariantNodeConversion);

::MongoExtensionStatus* ExtensionAggStageParseNode::_extExpand(
    const ::MongoExtensionAggStageParseNode* parseNode,
    ::MongoExtensionExpandedArray* expanded) noexcept {
    return wrapCXXAndConvertExceptionToStatus([&]() {
        const auto& impl = static_cast<const ExtensionAggStageParseNode*>(parseNode)->getImpl();
        const auto expandedSize = impl.getExpandedSize();
        sdk_tassert(11113801,
                    (str::stream()
                     << "MongoExtensionExpandedArray.size must equal required size: "
                     << "got " << expanded->size << ", but required " << expandedSize),
                    expanded->size == expandedSize);

        auto expandedNodes = impl.expand();
        sdk_uassert(11113802,
                    (str::stream() << "AggStageParseNode expand() returned a different "
                                      "number of elements than getExpandedSize(): returned "
                                   << expandedNodes.size() << ", but required " << expandedSize),
                    expandedNodes.size() == expandedSize);

        // If we exit early, destroy the ABI nodes and null any raw pointers written to the
        // caller's buffer.
        size_t filled = 0;
        ScopeGuard guard([&]() noexcept {
            // Destroy elements already written to the expanded array.
            for (size_t i = 0; i < filled; ++i) {
                destroyArrayElement(expanded->elements[i]);
            }
            // Elements not yet written to the expanded array are still owned by the handle vector
            // and will be destroyed there instead.
        });

        // Populate the caller's buffer directly with raw pointers to nodes.
        for (size_t i = 0; i < expandedSize; ++i) {
            if (MONGO_unlikely(failVariantNodeConversion.shouldFail())) {
                sdk_uasserted(11197200,
                              "Injected failure in VariantNode conversion to ExpandedArrayElement");
            }
            auto& dst = expanded->elements[i];
            std::visit(ConsumeVariantNodeToAbi{dst}, std::move(expandedNodes[i]));
            ++filled;
        }
        guard.dismiss();
    });
}
}  // namespace mongo::extension::sdk
