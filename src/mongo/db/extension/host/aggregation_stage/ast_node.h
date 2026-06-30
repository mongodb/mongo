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
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <functional>
#include <list>
#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class DocumentSource;
class ExpressionContext;
class LiteParsedDocumentSource;
class LiteParsedInternalSearchIdLookUp;
struct LiteParserOptions;
class NamespaceString;
}  // namespace mongo

/**
 * A host `AggStageAstNode` should be allocated for internal stages that we don't expect to
 * be written in user pipelines and don't participate in query shape.
 */
namespace mongo::extension::host {

/**
 * Host-defined AST node.
 *
 * Abstract base for nodes that a host-defined parse node can expand into. All behavior is
 * type-specific; concrete subclasses know how to re-parse themselves into a
 * LiteParsedDocumentSource (for privilege/namespace checking) and into executable
 * DocumentSource(s).
 */
class AggStageAstNode {
public:
    virtual ~AggStageAstNode() = default;

    /**
     * Gets the stage name.
     */
    virtual const std::string& getName() const = 0;

    /**
     * Re-parses this node into an owned LiteParsedDocumentSource using the expansion-time nss and
     * options. The default re-parses buildStageBson(); subclasses may override to inject node-held
     * state (e.g. an extension DPL callback) that cannot survive the BSON round-trip.
     */
    virtual std::unique_ptr<LiteParsedDocumentSource> expandToLiteParsed(
        const NamespaceString& nss, const LiteParserOptions& options) const;

    /**
     * Expands into executable DocumentSource(s). The default re-parses buildStageBson(); subclasses
     * may override to inject node-held state (e.g. an extension DPL callback) that cannot survive a
     * BSON round-trip.
     */
    virtual std::list<boost::intrusive_ptr<DocumentSource>> expandToDocumentSource(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const;

    /**
     * Returns an independent copy of this node.
     */
    virtual std::unique_ptr<AggStageAstNode> clone() const = 0;

protected:
    /**
     * Reconstructs the full stage BSON this node represents. Both expansion paths re-parse this
     * BSON, so subclasses only need to supply it.
     */
    virtual BSONObj buildStageBson() const = 0;
};

/**
 * idLookup-backed AST node. Wraps a $_internalSearchIdLookup LiteParsed and reconstructs the stage
 * BSON from it on demand.
 */
class IdLookupAstNode final : public AggStageAstNode {
public:
    explicit IdLookupAstNode(std::unique_ptr<LiteParsedInternalSearchIdLookUp> lp);

    const std::string& getName() const override;

    std::unique_ptr<AggStageAstNode> clone() const override;

private:
    /**
     * Reconstructs the full stage BSON from the underlying $_internalSearchIdLookup spec.
     */
    BSONObj buildStageBson() const override;

    std::unique_ptr<LiteParsedInternalSearchIdLookUp> _liteParsed;
};

/**
 * Shared, single-use RAII owner for an extension-provided distributedPlanLogic() callback, carried
 * by the DRM AST node.
 *
 * Ownership: holds an invoker (wrapping the extension callback) and an optional deleter (wrapping
 * the extension's cleanup hook) in a shared control block. The deleter runs exactly once when the
 * last shared copy is destroyed, no matter how many times the owner is copied (e.g. when the AST
 * node is cloned).
 *
 * Invocation: getOrInvoke() calls the invoker at most once and caches the parsed result, so the
 * planner's repeated distributedPlanLogic() queries reuse a single invocation.
 */
class DPLCallbackOwner {
public:
    using CallbackInvoker =
        std::function<::MongoExtensionStatus*(ExpressionContext* expCtx,
                                              ::MongoExtensionByteBuf** rawSort,
                                              ::MongoExtensionByteBuf** rawMerge)>;

    DPLCallbackOwner() = default;
    DPLCallbackOwner(CallbackInvoker invoker, std::function<void()> deleter = {});

    /**
     * True if an extension DPL callback is present.
     */
    bool hasCallback() const;

    /**
     * Invokes the extension callback at most once, caching and returning its parsed, validated
     * sharded-plan output. Must only be called when hasCallback() is true.
     *
     * 'expCtx' supplies the host query execution context (opCtx, namespace) handed to the extension
     * callback, which it may need to reach the host (e.g. $search's mongot network call). It is
     * only consulted on the single invocation; later cached calls ignore it.
     */
    const DocumentSourceInternalDocumentResultsAndMetadata::ShardedPlanSpec& getOrInvoke(
        ExpressionContext* expCtx) const;

private:
    struct State;
    std::shared_ptr<State> _state;
};

/**
 * DRM ($_internalDocumentResultsAndMetadata) AST node. Stores the full stage BSONObj and a
 * single-use DPL callback owner directly, without constructing a LiteParsedDocumentSource (which
 * would require nss and parsed pipelines not available at construction time).
 *
 * On expansion, if a DPL callback is present, expandToDocumentSource() wraps it into the stage's
 * sharded-plan source (setShardedPlan) so distributedPlanLogic() can produce the sharded
 * merge sort pattern / metadata merge pipeline.
 */
class DocumentResultsAndMetadataAstNode final : public AggStageAstNode {
public:
    DocumentResultsAndMetadataAstNode(BSONObj stageBson, DPLCallbackOwner dplOwner);

    const std::string& getName() const override;

    std::unique_ptr<AggStageAstNode> clone() const override;

    std::unique_ptr<LiteParsedDocumentSource> expandToLiteParsed(
        const NamespaceString& nss, const LiteParserOptions& options) const override;

    std::list<boost::intrusive_ptr<DocumentSource>> expandToDocumentSource(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const override;

private:
    BSONObj buildStageBson() const override;

    // Wraps the shared DPL callback owner into a DocResultsShardedPlanProvider closure. Shared by
    // both expansion paths so the wrapping logic (capture-by-shared-owner, getOrInvoke) lives in
    // one place.
    DocResultsShardedPlanProvider makeShardedPlanProvider() const;

    std::string _stageName;
    BSONObj _stageBson;
    DPLCallbackOwner _dplOwner;
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
 * WARNING: Do not use the HostAggStageAstNodeAdapter vtable function `promote`. It is
 * unimplemented. Future work will enable a HostAggStageAstNodeAdapter to promote directly into a
 * host-implemented LiteParsedExpandedDocumentSource and thus provide an implementation for
 * `promote`.
 */
class HostAggStageAstNodeAdapter final : public ::MongoExtensionAggStageAstNode {
public:
    HostAggStageAstNodeAdapter(std::unique_ptr<AggStageAstNode> astNode)
        : ::MongoExtensionAggStageAstNode(&VTABLE), _astNode(std::move(astNode)) {}

    ~HostAggStageAstNodeAdapter() = default;

    /**
     * Re-parses the underlying AST node into an owned LiteParsedDocumentSource.
     */
    inline std::unique_ptr<LiteParsedDocumentSource> expandToLiteParsed(
        const NamespaceString& nss, const LiteParserOptions& options) const {
        return _astNode->expandToLiteParsed(nss, options);
    }

    /**
     * Expands the underlying AST node into executable DocumentSource(s).
     */
    inline std::list<boost::intrusive_ptr<DocumentSource>> expandToDocumentSource(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
        return _astNode->expandToDocumentSource(expCtx);
    }

    /**
     * Gets the stage name from the underlying AggStageAstNode.
     */
    inline const std::string& getStageName() const {
        return _astNode->getName();
    }

    /**
     * Specifies whether the provided AST node was allocated by the host.
     *
     * Since ExtensionAggStageAstNodeAdapter and HostAggStageAstNodeAdapter implement the same
     * vtable, this function is necessary for differentiating between host- and extension-allocated
     * AST nodes.
     *
     * Use this function to check if an AST node is host-allocated before casting a
     * MongoExtensionAggStageAstNode to a HostAggStageAstNodeAdapter.
     */
    static inline bool isHostAllocated(::MongoExtensionAggStageAstNode& astNode) {
        return astNode.vtable == &VTABLE;
    }

    static ::MongoExtensionAggStageAstNodeVTable getVTable() {
        return VTABLE;
    }

private:
    const AggStageAstNode& getImpl() const noexcept {
        return *_astNode;
    }

    AggStageAstNode& getImpl() noexcept {
        return *_astNode;
    }

    static void _hostDestroy(::MongoExtensionAggStageAstNode* astNode) noexcept {
        delete static_cast<HostAggStageAstNodeAdapter*>(astNode);
    }

    static ::MongoExtensionByteView _hostGetName(
        const ::MongoExtensionAggStageAstNode* astNode) noexcept {
        return stringDataAsByteView(
            static_cast<const HostAggStageAstNodeAdapter*>(astNode)->getStageName());
    }

    static ::MongoExtensionStatus* _hostGetProperties(
        const ::MongoExtensionAggStageAstNode* astNode,
        ::MongoExtensionByteBuf** properties) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11347801,
                      "_hostGetProperties should not be called. Ensure that astNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static ::MongoExtensionStatus* _hostPromote(
        const ::MongoExtensionAggStageAstNode* astNode,
        const ::MongoExtensionCatalogContext* catalogContext,
        ::MongoExtensionLogicalAggStage** logicalStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11133600,
                      "_hostPromote should not be called. Ensure that astNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static ::MongoExtensionStatus* _hostClone(const ::MongoExtensionAggStageAstNode* astNode,
                                              ::MongoExtensionAggStageAstNode** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            auto* hostAstNode = static_cast<const HostAggStageAstNodeAdapter*>(astNode);
            *output = new HostAggStageAstNodeAdapter(hostAstNode->getImpl().clone());
        });
    }

    static ::MongoExtensionStatus* _hostGetFirstStageViewApplicationPolicy(
        const ::MongoExtensionAggStageAstNode* astNode,
        ::MongoExtensionFirstStageViewApplicationPolicy* output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            tasserted(11507401,
                      "_hostGetFirstStageViewApplicationPolicy should not be called. Ensure that "
                      "astNode is extension-allocated, not host-allocated");
        });
    }

    static ::MongoExtensionStatus* _hostBindResolvedNamespace(
        ::MongoExtensionAggStageAstNode* astNode,
        const ::MongoExtensionResolvedNamespace* resolvedNamespace) noexcept {
        return wrapCXXAndConvertExceptionToStatus([&]() {
            tasserted(11507501,
                      "_hostBindResolvedNamespace should not be called. Ensure that astNode is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static constexpr ::MongoExtensionAggStageAstNodeVTable VTABLE = {
        .destroy = &_hostDestroy,
        .get_name = &_hostGetName,
        .get_properties = &_hostGetProperties,
        .promote = &_hostPromote,
        .clone = &_hostClone,
        .get_first_stage_view_application_policy = &_hostGetFirstStageViewApplicationPolicy,
        .bind_resolved_namespace = &_hostBindResolvedNamespace};

    std::unique_ptr<AggStageAstNode> _astNode;
};
};  // namespace mongo::extension::host
