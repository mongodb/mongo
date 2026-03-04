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
#include "mongo/db/extension/host_connector/adapter/query_shape_opts_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>

/**
 * A host `AggStageParseNode` should be allocated for server stages that can be written in a
 * user pipeline and can participate in query shape.
 */
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
class AggStageParseNode {
public:
    AggStageParseNode(BSONObj spec) : _spec(spec.getOwned()) {}

    ~AggStageParseNode() = default;

    /**
     * Gets the BSON representation of the host-defined aggregation stage.
     *
     * The returned BSONObj is owned by this class.
     */
    inline BSONObj getBsonSpec() const {
        return _spec;
    }

    inline BSONObj toBsonForLog() const {
        return _spec;
    }

private:
    BSONObj _spec;
};

/**
 * Boundary object representation of a ::MongoExtensionAggStageParseNode.
 *
 * This class abstracts the C++ implementation of the extension and provides the interface at the
 * API boundary which will be called upon by the host. The static VTABLE member points to static
 * methods which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggStageParseNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggStageParseNode.
 *
 * WARNING: Do not use the HostAggStageParseNodeAdapter vtable functions. They are unimplemented.
 * Future work will enable a HostAggStageParseNodeAdapter to expand into a host-implemented
 * LiteParsedDocumentSource and thus provide an implementation for `getQueryShape`, `expand`, etc.
 */
class HostAggStageParseNodeAdapter final : public ::MongoExtensionAggStageParseNode {
public:
    HostAggStageParseNodeAdapter(std::unique_ptr<AggStageParseNode> parseNode)
        : ::MongoExtensionAggStageParseNode(&VTABLE), _parseNode(std::move(parseNode)) {}

    ~HostAggStageParseNodeAdapter() = default;

    /**
     * The underlying bytes are owned by this AggStageParseNode. The returned BSONObj
     * is safe to use while either the returned BSONObj or this AggStageParseNode remains alive.
     * If you need the bytes to outlive the parse node, call getOwned() on the returned value.
     */
    inline BSONObj getBsonSpec() const {
        return _parseNode->getBsonSpec();
    }

    /**
     * Specifies whether the provided parse node was allocated by the host.
     *
     * Since ExtensionAggStageParseNodeAdapter and HostAggStageParseNodeAdapter implement the same
     * vtable, this function is necessary for differentiating between host- and extension-allocated
     * parse nodes.
     *
     * Use this function to check if a parse node is host-allocated before casting a
     * MongoExtensionAggStageParseNode to a HostAggStageParseNodeAdapter.
     */
    static inline bool isHostAllocated(::MongoExtensionAggStageParseNode& parseNode) {
        return parseNode.vtable == &VTABLE;
    }

    static ::MongoExtensionAggStageParseNodeVTable getVTable() {
        return VTABLE;
    }

private:
    const AggStageParseNode& getImpl() const noexcept {
        return *_parseNode;
    }

    AggStageParseNode& getImpl() noexcept {
        return *_parseNode;
    }

    static void _hostDestroy(::MongoExtensionAggStageParseNode* parseNode) noexcept {
        delete static_cast<HostAggStageParseNodeAdapter*>(parseNode);
    }

    static ::MongoExtensionByteView _hostGetName(
        const ::MongoExtensionAggStageParseNode* parseNode) noexcept {
        return stringDataAsByteView(static_cast<const HostAggStageParseNodeAdapter*>(parseNode)
                                        ->getBsonSpec()
                                        .firstElementFieldNameStringData());
    }

    static ::MongoExtensionStatus* _hostGetQueryShape(
        const ::MongoExtensionAggStageParseNode* parseNode,
        const ::MongoExtensionHostQueryShapeOpts* ctx,
        ::MongoExtensionByteBuf** queryShape) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            uassert(11717600, "Received invalid query shape opts pointer", ctx != nullptr);
            uassert(11717601,
                    "A host parse node can only use host-generated query shape opts",
                    host_connector::QueryShapeOptsAdapter::isHostAllocated(*ctx));

            *queryShape = nullptr;
            auto bsonSpec =
                static_cast<const HostAggStageParseNodeAdapter*>(parseNode)->getBsonSpec();

            // TODO SERVER-117619: Remove expression context from QueryShapeOptsAdapter once you can
            // generate a query shape without going to DocumentSource.
            const auto* optsAdapter =
                static_cast<const host_connector::QueryShapeOptsAdapter*>(ctx);

            auto parsedStage = DocumentSource::parse(optsAdapter->getExpCtx(), bsonSpec);
            uassert(11717602,
                    "It is not supported for a host parse node to generate query shape for a "
                    "desugar stage",
                    parsedStage.size() == 1);

            Value serialized = parsedStage.front()->serialize(*optsAdapter->getOptsImpl());
            *queryShape = new ByteBuf(serialized.getDocument().toBson());
        });
    };

    static ::MongoExtensionStatus* _hostExpand(
        const ::MongoExtensionAggStageParseNode* parseNode,
        ::MongoExtensionExpandedArrayContainer** outExpanded) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(10977801,
                      "_hostExpand should not be called. Ensure that parseNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static ::MongoExtensionStatus* _hostClone(const ::MongoExtensionAggStageParseNode* parseNode,
                                              ::MongoExtensionAggStageParseNode** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            auto* hostParseNode = static_cast<const HostAggStageParseNodeAdapter*>(parseNode);
            auto clonedParseNode =
                std::make_unique<AggStageParseNode>(hostParseNode->getBsonSpec());
            *output = new HostAggStageParseNodeAdapter(std::move(clonedParseNode));
        });
    }

    static ::MongoExtensionStatus* _hostToBsonForLog(
        const ::MongoExtensionAggStageParseNode* parseNode,
        ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            *output = nullptr;
            const auto& impl =
                static_cast<const HostAggStageParseNodeAdapter*>(parseNode)->getImpl();
            // Allocate a buffer on the heap. Ownership is transferred to the caller.
            *output = new ByteBuf(impl.toBsonForLog());
        });
    }

    static constexpr ::MongoExtensionAggStageParseNodeVTable VTABLE = {
        .destroy = &_hostDestroy,
        .get_name = &_hostGetName,
        .get_query_shape = &_hostGetQueryShape,
        .expand = &_hostExpand,
        .clone = &_hostClone,
        .to_bson_for_log = &_hostToBsonForLog};

    std::unique_ptr<AggStageParseNode> _parseNode;
};
};  // namespace mongo::extension::host
