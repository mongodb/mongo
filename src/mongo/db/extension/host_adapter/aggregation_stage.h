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
#include "mongo/db/extension/host_adapter/handle.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

#include <vector>

#include <absl/base/nullability.h>

namespace mongo::extension::host_adapter {

/**
 * LogicalAggregationStageHandle is an owned handle wrapper around a
 * MongoExtensionLogicalAggregationStage.
 */
class LogicalAggregationStageHandle : public OwnedHandle<::MongoExtensionLogicalAggregationStage> {
public:
    LogicalAggregationStageHandle(::MongoExtensionLogicalAggregationStage* ptr)
        : OwnedHandle<::MongoExtensionLogicalAggregationStage>(ptr) {
        _assertValidVTable();
    }

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {}
};

/**
 * AggregationStageDescriptorHandle is a wrapper around a
 * MongoExtensionAggregationStageDescriptor.
 */
class AggregationStageDescriptorHandle
    : public UnownedHandle<const ::MongoExtensionAggregationStageDescriptor> {
public:
    AggregationStageDescriptorHandle(
        absl::Nonnull<const ::MongoExtensionAggregationStageDescriptor*> descriptor)
        : UnownedHandle<const ::MongoExtensionAggregationStageDescriptor>(descriptor) {
        _assertValidVTable();
    }

    /**
     * Returns a StringData containing the name of this aggregation stage.
     */
    StringData getName() const {
        auto stringView = sdk::byteViewAsStringView(vtable().get_name(get()));
        return StringData{stringView.data(), stringView.size()};
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
    LogicalAggregationStageHandle parse(BSONObj stageBson) const;

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(10930102,
                "ExtensionAggregationStageDescriptor 'get_name' is null",
                vtable.get_name != nullptr);
        tassert(10930103,
                "ExtensionAggregationStageDescriptor 'get_type' is null",
                vtable.get_type != nullptr);
        tassert(10930104,
                "ExtensionAggregationStageDescriptor 'parse' is null",
                vtable.parse != nullptr);
    }
};

/**
 * AggregationStageAstNodeHandle is an owned handle wrapper around a
 * MongoExtensionAggregationStageAstNode.
 */
class AggregationStageAstNodeHandle : public OwnedHandle<::MongoExtensionAggregationStageAstNode> {
public:
    AggregationStageAstNodeHandle(::MongoExtensionAggregationStageAstNode* ptr)
        : OwnedHandle<::MongoExtensionAggregationStageAstNode>(ptr) {
        _assertValidVTable();
    }

    /**
     * Returns a logical stage with the stage's runtime implementation of the optimization
     * interface.
     *
     * On success, the logical stage is returned and belongs to the caller.
     * On failure, the error triggers an assertion.
     *
     */
    LogicalAggregationStageHandle bind() const;

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(
            11113700, "ExtensionAggregationStageAstNode 'bind' is null", vtable.bind != nullptr);
    }
};

class AggregationStageParseNodeHandle;
using VariantNodeHandle =
    std::variant<AggregationStageParseNodeHandle, AggregationStageAstNodeHandle>;

/**
 * AggregationStageParseNodeHandle is a wrapper around a
 * MongoExtensionAggregationStageParseNode.
 */
class AggregationStageParseNodeHandle
    : public OwnedHandle<::MongoExtensionAggregationStageParseNode> {
public:
    AggregationStageParseNodeHandle(
        absl::Nonnull<::MongoExtensionAggregationStageParseNode*> parseNode)
        : OwnedHandle<::MongoExtensionAggregationStageParseNode>(parseNode) {
        _assertValidVTable();
    }

    // TODO(SERVER-111368): Add getQueryShape().

    /**
     * Expands this parse node into its child nodes and returns them as a vector of host-side
     * handles.
     *
     * On success, the returned vector contains one or more handles (ParseNodeHandle or
     * AstNodeHandle) and ownership of the underlying nodes is transferred to the caller.
     *
     * On failure, the call triggers an assertion and no ownership is transferred.
     *
     * The caller is responsible for managing the lifetime of the returned handles.
     */
    std::vector<VariantNodeHandle> expand() const;

protected:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(10977601,
                "ExtensionAggregationStageParseNode 'get_query_shape' is null",
                vtable.get_query_shape != nullptr);
        tassert(11113800,
                "ExtensionAggregationStageParseNode 'get_expanded_size' is null",
                vtable.get_expanded_size != nullptr);
        tassert(10977602,
                "ExtensionAggregationStageParseNode 'expand' is null",
                vtable.expand != nullptr);
    }

private:
    /**
     * Returns the number of elements the parse node will produce when expanded. This value is used
     * by the host to size the ExpandedArray.
     */
    size_t getExpandedSize() const {
        return vtable().get_expanded_size(get());
    }
};

}  // namespace mongo::extension::host_adapter
