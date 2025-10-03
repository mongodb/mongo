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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::extension::host {

/**
 * Host-defined parse node.
 *
 * This class wraps the BSON specification of an existing server/MQL stage an extension is
 * desugaring into. During expansion, the host is responsible for detecting any host-defined parse
 * nodes and retrieving the BSON specification for further processing. The BSON will then be parsed
 * into a LiteParsedDocumentSource for preliminary validation and later into a fully constructed
 * DocumentSource stage during full pipeline parsing.
 */
class AggregationStageParseNode {
public:
    AggregationStageParseNode(BSONObj spec) : _spec(spec.getOwned()) {}

    ~AggregationStageParseNode() = default;

    /**
     * Gets the BSON representation of the host-defined aggregation stage.
     *
     * The returned BSONObj is owned by this class.
     */
    inline BSONObj getBsonSpec() const {
        return _spec;
    }

private:
    BSONObj _spec;
};

/**
 * Boundary object representation of a ::MongoExtensionAggregationStageParseNode.
 *
 * This class abstracts the C++ implementation of the extension and provides the interface at the
 * API boundary which will be called upon by the host. The static VTABLE member points to static
 * methods which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggregationStageParseNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggregationStageParseNode.
 *
 * WARNING: Do not use the HostAggregationStageParseNode vtable functions. They are unimplemented.
 * Future work will enable a HostAggregationStageParseNode to expand into a host-implemented
 * LiteParsedDocumentSource and thus provide an implementation for `getQueryShape`, `expand`, etc.
 */
class HostAggregationStageParseNode final : public ::MongoExtensionAggregationStageParseNode {
public:
    HostAggregationStageParseNode(std::unique_ptr<AggregationStageParseNode> parseNode)
        : ::MongoExtensionAggregationStageParseNode(&VTABLE), _parseNode(std::move(parseNode)) {}

    ~HostAggregationStageParseNode() = default;

    /**
     * Gets the BSON representation of the host-defined aggregation stage stored in the underlying
     * AggregationStageParseNode.
     *
     * The returned BSONObj is owned by the underlying AggregationStageParseNode. If you want the
     * BSON to outlive this class instance, you should create your own copy.
     */
    inline BSONObj getBsonSpec() const {
        return _parseNode->getBsonSpec();
    }

    /**
     * Specifies whether the provided parse node was allocated by the host.
     *
     * Since ExtensionAggregationStageParseNode and HostAggregationStageParseNode implement the same
     * vtable, this function is necessary for differentiating between host- and extension-allocated
     * parse nodes.
     *
     * Use this function to check if a parse node is host-allocated before casting a
     * MongoExtensionAggregationStageParseNode to a HostAggregationStageParseNode.
     */
    static inline bool isHostAllocated(::MongoExtensionAggregationStageParseNode& parseNode) {
        return parseNode.vtable == &VTABLE;
    }

private:
    const AggregationStageParseNode& getImpl() const noexcept {
        return *_parseNode;
    }

    AggregationStageParseNode& getImpl() noexcept {
        return *_parseNode;
    }

    static void _hostDestroy(::MongoExtensionAggregationStageParseNode* parseNode) noexcept {
        delete static_cast<HostAggregationStageParseNode*>(parseNode);
    }

    static ::MongoExtensionStatus* _hostGetQueryShape(
        const ::MongoExtensionAggregationStageParseNode* parseNode,
        const ::MongoHostQueryShapeOpts* ctx,
        ::MongoExtensionByteBuf** queryShape) noexcept {
        return sdk::enterCXX([]() {
            tasserted(10977800,
                      "_hostGetQueryShape should not be called. Ensure that parseNode is "
                      "extension-allocated, not host-allocated.");
        });
    };

    static size_t _hostGetExpandedSize(
        const ::MongoExtensionAggregationStageParseNode* parseNode) noexcept {
        // This function should not be called, but tassert cannot be used because C++ errors must
        // not propagate across the C ABI boundary and the return type of this function is size_t.
        // Since the invariant that get_expansion_size returns a size > 0 is enforced elsewhere,
        // this return value still results in an error.
        return 0;
    }

    static ::MongoExtensionStatus* _hostExpand(
        const ::MongoExtensionAggregationStageParseNode* parseNode,
        ::MongoExtensionExpandedArray* outExpanded) noexcept {
        return sdk::enterCXX([]() {
            tasserted(10977801,
                      "_hostExpand should not be called. Ensure that parseNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static constexpr ::MongoExtensionAggregationStageParseNodeVTable VTABLE{
        .destroy = &_hostDestroy,
        .get_query_shape = &_hostGetQueryShape,
        .get_expanded_size = &_hostGetExpandedSize,
        .expand = &_hostExpand};

    std::unique_ptr<AggregationStageParseNode> _parseNode;
};
};  // namespace mongo::extension::host
