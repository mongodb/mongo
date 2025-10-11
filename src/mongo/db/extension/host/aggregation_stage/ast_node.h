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
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>

/**
 * A host `AggStageAstNode` should be allocated for internal stages that we don't expect to
 * be written in user pipelines and don't participate in query shape.
 */
namespace mongo::extension::host {

/**
 * Host-defined AST node.
 *
 * Wraps a LiteParsedDocumentSource and serves as a node that a host-defined parse node can expand
 * into. Currently only supports $_internalSearchIdLookup.
 */
class AggStageAstNode {
public:
    AggStageAstNode(std::unique_ptr<LiteParsedDocumentSource> lp) : _liteParsed(std::move(lp)) {}

    ~AggStageAstNode() = default;

    /**
     * Gets the BSON representation of an $_internalSearchIdLookup stage.
     * The returned BSONObj is owned by this class.
     */
    BSONObj getIdLookupSpec() const {
        const auto* idLookup =
            dynamic_cast<const DocumentSourceInternalSearchIdLookUp::LiteParsed*>(
                _liteParsed.get());
        uassert(11160700,
                str::stream() << "Expected $_internalSearchIdLookup stage, but got: "
                              << _liteParsed->getParseTimeName(),
                idLookup);
        return idLookup->getBsonSpec();
    }

private:
    std::unique_ptr<LiteParsedDocumentSource> _liteParsed;
};

/**
 * Boundary object representation of a ::MongoExtensionAggStageAstNode.
 *
 * This class abstracts the C++ implementation of the extension and provides the interface at the
 * API boundary which will be called upon by the host. The static VTABLE member points to static
 * methods which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionAggStageAstNode interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the AggStageAstNode.
 *
 * WARNING: Do not use the HostAggStageAstNode vtable function `bind`. It is
 * unimplemented. Future work will enable a HostAggStageAstNode to bind directly into a
 * host-implemented LiteParsedExpandedDocumentSource and thus provide an implementation for `bind`.
 */
class HostAggStageAstNode final : public ::MongoExtensionAggStageAstNode {
public:
    HostAggStageAstNode(std::unique_ptr<AggStageAstNode> astNode)
        : ::MongoExtensionAggStageAstNode(&VTABLE), _astNode(std::move(astNode)) {}

    ~HostAggStageAstNode() = default;

    /**
     * Gets the BSON representation of the $_internalSearchIdLookup stage stored in the underlying
     * AggStageAstNode. This will be extended to additional internal stage types in the
     * future.
     *
     * The returned BSONObj is owned by the underlying AggStageAstNode. If you want the
     * BSON to outlive this class instance, you should create your own copy.
     */
    inline BSONObj getIdLookupSpec() const {
        return _astNode->getIdLookupSpec();
    }

    /**
     * Specifies whether the provided AST node was allocated by the host.
     *
     * Since ExtensionAggStageAstNode and HostAggStageAstNode implement the same
     * vtable, this function is necessary for differentiating between host- and extension-allocated
     * AST nodes.
     *
     * Use this function to check if an AST node is host-allocated before casting a
     * MongoExtensionAggStageAstNode to a HostAggStageAstNode.
     */
    static inline bool isHostAllocated(::MongoExtensionAggStageAstNode& astNode) {
        return astNode.vtable == &VTABLE;
    }

private:
    const AggStageAstNode& getImpl() const noexcept {
        return *_astNode;
    }

    AggStageAstNode& getImpl() noexcept {
        return *_astNode;
    }

    static void _hostDestroy(::MongoExtensionAggStageAstNode* astNode) noexcept {
        delete static_cast<HostAggStageAstNode*>(astNode);
    }

    static ::MongoExtensionByteView _hostGetName(
        const ::MongoExtensionAggStageAstNode* astNode) noexcept {
        return stringDataAsByteView(static_cast<const HostAggStageAstNode*>(astNode)
                                        ->getIdLookupSpec()
                                        .firstElementFieldNameStringData());
    }

    static ::MongoExtensionStatus* _hostBind(
        const ::MongoExtensionAggStageAstNode* astNode,
        ::MongoExtensionLogicalAggStage** logicalStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11133600,
                      "_hostBind should not be called. Ensure that astNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static constexpr ::MongoExtensionAggStageAstNodeVTable VTABLE{
        .destroy = &_hostDestroy, .get_name = &_hostGetName, .bind = &_hostBind};

    std::unique_ptr<AggStageAstNode> _astNode;
};
};  // namespace mongo::extension::host
