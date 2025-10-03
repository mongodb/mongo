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
#include "mongo/db/extension/host_adapter/aggregation_stage.h"

#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/util/fail_point.h"

namespace mongo::extension::host_adapter {
MONGO_FAIL_POINT_DEFINE(failExtensionExpand);

LogicalAggregationStageHandle AggregationStageDescriptorHandle::parse(BSONObj stageBson) const {
    ::MongoExtensionLogicalAggregationStage* logicalStagePtr;
    // The API's contract mandates that logicalStagePtr will only be allocated if status is OK.
    sdk::enterC(
        [&]() { return vtable().parse(get(), sdk::objAsByteView(stageBson), &logicalStagePtr); });
    return LogicalAggregationStageHandle(logicalStagePtr);
}

std::vector<VariantNodeHandle> AggregationStageParseNodeHandle::expand() const {
    // Host allocates buffer with the expected size.
    const auto expandedSize = getExpandedSize();
    tassert(
        11113803, "AggregationStageParseNode getExpandedSize() must be >= 1", expandedSize >= 1);
    std::vector<::MongoExtensionExpandedArrayElement> buf{expandedSize};
    ::MongoExtensionExpandedArray expandedArray{expandedSize, buf.data()};

    sdk::enterC([&] { return vtable().expand(get(), &expandedArray); });

    // This guard provides a best-effort cleanup in the case of an exception.
    //
    // - `transferredCount` tracks how many elements from the front of `buf` have been
    //   successfully wrapped into RAII handles and had their raw pointers nulled.
    // - If an exception occurs while constructing handles (e.g., OOM in `emplace_back` or a bad
    //   vtable), we destroy only the elements that have not yet been transferred
    //   ([transferredCount, expandedSize)).
    size_t transferredCount{0};
    ScopeGuard guard([&]() {
        for (size_t idx = transferredCount; idx < expandedSize; ++idx) {
            auto& elt = expandedArray.elements[idx];
            switch (elt.type) {
                case kParseNode: {
                    if (elt.parse && elt.parse->vtable && elt.parse->vtable->destroy) {
                        elt.parse->vtable->destroy(elt.parse);
                    }
                    break;
                }
                case kAstNode: {
                    if (elt.ast && elt.ast->vtable && elt.ast->vtable->destroy) {
                        elt.ast->vtable->destroy(elt.ast);
                    }
                    break;
                }
                default:
                    // Memory is leaked if the type tag is invalid, but this only happens if the
                    // extension violates the API contract.
                    break;
            }
        }
    });

    // Transfer ownership of each element into RAII handles and build the result vector.
    std::vector<VariantNodeHandle> expandedVec;
    expandedVec.reserve(expandedSize);
    for (auto& elt : buf) {
        if (MONGO_unlikely(failExtensionExpand.shouldFail())) {
            uasserted(11113805, "Injected failure in expand() during handle transfer");
        }

        switch (elt.type) {
            case kParseNode: {
                expandedVec.emplace_back(elt.parse);
                elt.parse = nullptr;
                break;
            }
            case kAstNode: {
                expandedVec.emplace_back(elt.ast);
                elt.ast = nullptr;
                break;
            }
            default:
                tasserted(11113804, "ExpandedArray element has invalid type tag");
                break;
        }
        ++transferredCount;
    }

    guard.dismiss();
    return expandedVec;
}

LogicalAggregationStageHandle AggregationStageAstNodeHandle::bind() const {
    ::MongoExtensionLogicalAggregationStage* logicalStagePtr;

    // The API's contract mandates that logicalStagePtr will only be allocated if status is OK.
    sdk::enterC([&]() { return vtable().bind(get(), &logicalStagePtr); });

    return LogicalAggregationStageHandle(logicalStagePtr);
}
}  // namespace mongo::extension::host_adapter
