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
#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/host/extension_status.h"
#include "mongo/db/extension/host/handle.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"

#include <absl/base/nullability.h>

namespace mongo::extension::host {

/**
 * ExtensionLogicalAggregationStageHandle is an owned handle wrapper around a
 * MongoExtensionLogicalAggregationStage.
 */
class ExtensionLogicalAggregationStageHandle
    : public OwnedHandle<::MongoExtensionLogicalAggregationStage> {
public:
    ExtensionLogicalAggregationStageHandle(::MongoExtensionLogicalAggregationStage* ptr)
        : OwnedHandle<::MongoExtensionLogicalAggregationStage>(ptr) {}
};

/**
 * ExtensionAggregationStageDescriptorHandle is a wrapper around a
 * MongoExtensionAggregationStageDescriptor.
 */
class ExtensionAggregationStageDescriptorHandle
    : public UnownedHandle<const ::MongoExtensionAggregationStageDescriptor> {
public:
    ExtensionAggregationStageDescriptorHandle(
        absl::Nonnull<const ::MongoExtensionAggregationStageDescriptor*> descriptor)
        : UnownedHandle<const ::MongoExtensionAggregationStageDescriptor>(descriptor) {}

    /**
     * Returns a StringData containing the name of this aggregation stage.
     */
    StringData getName() const {
        return sdk::byteViewAsStringData(vtable().get_name(get()));
    }

    /**
     * Return the type for this stage.
     */
    MongoExtensionAggregationStageType getType() const {
        return vtable().get_type(get());
    }

    /**
     * Parse the user provided stage definition for this stage descriptor.
     *
     * stageBson contains a BSON document with a single (stageName, stageDefinition) element
     * tuple.
     *
     * On success, the logical stage is returned and belongs to the caller.
     * On failure, the error triggers an assertion.
     *
     */
    ExtensionLogicalAggregationStageHandle parse(BSONObj stageBson) const {
        ::MongoExtensionLogicalAggregationStage* logicalStagePtr;
        const auto& vtbl = vtable();
        auto* ptr = get();
        // The API's contract mandates that logicalStagePtr will only be allocated if status is OK.
        sdk::enterC(
            [&]() { return vtbl.parse(ptr, sdk::objAsByteView(stageBson), &logicalStagePtr); });
        return ExtensionLogicalAggregationStageHandle(logicalStagePtr);
    }
};
}  // namespace mongo::extension::host
