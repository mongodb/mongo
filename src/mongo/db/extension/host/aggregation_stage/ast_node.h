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

/**
 * A host `AggregationStageAstNode` should be allocated for internal stages that we don't expect to
 * be written in user pipelines and don't participate in query shape.
 */
namespace mongo::extension::host {

/**
 * Host-defined AST node.
 *
 * This class wraps the BSON specification for the internal stage $_internalSearchIdLookup and
 * serves as a node that a host-defined parse node can expand into.
 */
class AggregationStageAstNode {
public:
    AggregationStageAstNode(BSONObj spec) : _spec(spec.getOwned()) {}

    ~AggregationStageAstNode() = default;

    /**
     * Gets the BSON representation of the $_internalSearchIdLookup stage. This will be extended to
     * additional internal stage types in the future.
     *
     * The returned BSONObj is owned by this class.
     */
    inline BSONObj getIdLookupSpec() const {
        return _spec;
    }

private:
    BSONObj _spec;
};

/**
 * Boundary object representation of a ::MongoExtensionAggregationStageAstNode.
 *
 * This class abstracts the C++ implementation of the extension and provides the interface at the
 * API boundary which will be called upon by the host. The static VTABLE member points to static
 * methods which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggregationStageAstNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggregationStageAstNode.
 *
 * WARNING: Do not use the HostAggregationStageAstNode vtable function `bind`. It is
 * unimplemented. Future work will enable a HostAggregationStageAstNode to bind directly into a
 * host-implemented LiteParsedExpandedDocumentSource and thus provide an implementation for `bind`.
 */
class HostAggregationStageAstNode final : public ::MongoExtensionAggregationStageAstNode {
public:
    HostAggregationStageAstNode(std::unique_ptr<AggregationStageAstNode> astNode)
        : ::MongoExtensionAggregationStageAstNode(&VTABLE), _astNode(std::move(astNode)) {}

    ~HostAggregationStageAstNode() = default;

    /**
     * Gets the BSON representation of the $_internalSearchIdLookup stage stored in the underlying
     * AggregationStageAstNode. This will be extended to additional internal stage types in the
     * future.
     *
     * The returned BSONObj is owned by the underlying AggregationStageAstNode. If you want the
     * BSON to outlive this class instance, you should create your own copy.
     */
    inline BSONObj getIdLookupSpec() const {
        return _astNode->getIdLookupSpec();
    }

    /**
     * Specifies whether the provided AST node was allocated by the host.
     *
     * Since ExtensionAggregationStageAstNode and HostAggregationStageAstNode implement the same
     * vtable, this function is necessary for differentiating between host- and extension-allocated
     * AST nodes.
     *
     * Use this function to check if an AST node is host-allocated before casting a
     * MongoExtensionAggregationStageAstNode to a HostAggregationStageAstNode.
     */
    static inline bool isHostAllocated(::MongoExtensionAggregationStageAstNode& astNode) {
        return astNode.vtable == &VTABLE;
    }

private:
    const AggregationStageAstNode& getImpl() const noexcept {
        return *_astNode;
    }

    AggregationStageAstNode& getImpl() noexcept {
        return *_astNode;
    }

    static void _hostDestroy(::MongoExtensionAggregationStageAstNode* astNode) noexcept {
        delete static_cast<HostAggregationStageAstNode*>(astNode);
    }

    static ::MongoExtensionStatus* _hostBind(
        const ::MongoExtensionAggregationStageAstNode* astNode,
        ::MongoExtensionLogicalAggregationStage** logicalStage) noexcept {
        return sdk::enterCXX([]() {
            tasserted(11133600,
                      "_hostBind should not be called. Ensure that astNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static constexpr ::MongoExtensionAggregationStageAstNodeVTable VTABLE{.destroy = &_hostDestroy,
                                                                          .bind = &_hostBind};

    std::unique_ptr<AggregationStageAstNode> _astNode;
};
};  // namespace mongo::extension::host
