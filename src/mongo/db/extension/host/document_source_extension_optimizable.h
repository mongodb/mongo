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
#include "mongo/db/extension/host/aggregation_stage/ast_node.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/catalog_context.h"
#include "mongo/db/extension/host/extension_host_utils.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/ast_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host {

class DocumentSourceExtensionOptimizableTest;
class LoadExtensionsTest;
class LoadNativeVectorSearchTest;

using LiteParsedList = std::list<std::unique_ptr<LiteParsedDocumentSource>>;

// Custom StageParams classes for LPDSExpandable and LPDSExpanded that own a parseNode and astNode
// respectively.
class ExpandableStageParams : public StageParams {
public:
    ExpandableStageParams(AggStageParseNodeHandle parseNode) : _parseNode(std::move(parseNode)) {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

    AggStageParseNodeHandle releaseParseNode() {
        return std::move(_parseNode);
    }

private:
    AggStageParseNodeHandle _parseNode;
};


class ExpandedStageParams : public StageParams {
public:
    ExpandedStageParams(AggStageAstNodeHandle astNode) : _astNode(std::move(astNode)) {}

    static const Id& id;

    Id getId() const override {
        return id;
    }

    AggStageAstNodeHandle releaseAstNode() {
        return std::move(_astNode);
    }

private:
    AggStageAstNodeHandle _astNode;
};

/**
 * A DocumentSource implementation for an extension aggregation stage.
 * DocumentSourceExtensionOptimizable is the concrete implementation for extension stages that can
 * participate in query optimization and execution.
 */
class DocumentSourceExtensionOptimizable : public DocumentSource {
public:
    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageParseNode.
     */
    class LiteParsedExpandable : public LiteParsedDocumentSource {
    public:
        inline static constexpr int kMaxExpansionDepth = 10;

        static std::unique_ptr<LiteParsedDocumentSource> parse(AggStageDescriptorHandle descriptor,
                                                               const NamespaceString& nss,
                                                               const BSONElement& spec,
                                                               const LiteParserOptions& options) {
            auto parseNode = descriptor->parse(spec.wrap());
            return std::make_unique<LiteParsedExpandable>(spec, std::move(parseNode), nss, options);
        }

        LiteParsedExpandable(const BSONElement& spec,
                             AggStageParseNodeHandle parseNode,
                             const NamespaceString& nss,
                             const LiteParserOptions& options)
            : LiteParsedDocumentSource(spec),
              _parseNode(std::move(parseNode)),
              _nss(nss),
              _options(options),
              _expanded([&] {
                  auto expandedList = expand();
                  tassert(10905600,
                          "LiteParsedExpandable must not have an empty expanded pipeline",
                          !expandedList.empty());

                  return StageSpecs(std::make_move_iterator(expandedList.begin()),
                                    std::make_move_iterator(expandedList.end()));
              }()) {}

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<ExpandableStageParams>(_parseNode->clone());
        }

        /**
         * Return a copy to the pre-computed expanded pipeline.
         */
        StageSpecs getExpandedPipeline() const {
            StageSpecs cloned;
            cloned.reserve(_expanded.size());
            for (const auto& stage : _expanded) {
                cloned.push_back(stage->clone());
            }
            return cloned;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            PrivilegeVector privileges;
            for (const auto& lp : _expanded) {
                Privilege::addPrivilegesToPrivilegeVector(
                    &privileges, lp->requiredPrivileges(isMongos, bypassDocumentValidation));
            }
            return privileges;
        }

        bool isInitialSource() const override {
            return _expanded.front()->isInitialSource();
        }

        bool requiresAuthzChecks() const override {
            for (const auto& lp : _expanded) {
                if (lp->requiresAuthzChecks()) {
                    return true;
                }
            }
            return false;
        }

        std::unique_ptr<LiteParsedDocumentSource> clone() const override {
            return std::make_unique<LiteParsedExpandable>(
                getOriginalBson(), _parseNode->clone(), _nss, _options);
        }

        // TODO SERVER-116021 Remove this override when extensions can handle views through
        // ViewPolicy.
        bool isExtensionVectorSearchStage() const override;

        // Define how to desugar a LiteParsedExpandable.
        static LiteParsedDesugarer::StageExpander stageExpander;

    private:
        /**
         * Carries per-invocation validation state for recursive expansion performed by
         * LiteParsedExpandable. The state is instantiated at the top-level expand() and passed by
         * reference to recursive expandImpl() calls.
         */
        struct ExpansionState {
            // Tracker for the current expansion depth. This is only incremented when expansion
            // results in an ExtensionParseNode that requires further expansion.
            int currDepth = 0;

            // Ordered container of stageNames along the current expansion path.
            std::vector<std::string> expansionPath;

            // Tracker for seen stageNames. Provides O(1) cycle checking.
            stdx::unordered_set<std::string> seenStages;
        };

        /**
         * RAII frame that enforces recursive expansion depth constraints and cycle checking,
         * unwinding on exit.
         */
        class ExpansionValidationFrame;

        LiteParsedList expand();
        static LiteParsedList expandImpl(const AggStageParseNodeHandle& parseNodeHandle,
                                         ExpansionState& state,
                                         const NamespaceString& nss,
                                         const LiteParserOptions& options);

        const AggStageParseNodeHandle _parseNode;
        const NamespaceString _nss;
        const LiteParserOptions _options;
        const StageSpecs _expanded;
    };

    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageAstNode.
     *
     * NOTE: This class is instantiated when LiteParsedDesugarer expands LiteParsedExpandable. From
     * there, LiteParsedExpanded is converted to DocumentSourceExtensionOptimizable for
     * optimization.
     */
    class LiteParsedExpanded : public LiteParsedDocumentSource {
    public:
        LiteParsedExpanded(std::string stageName,
                           AggStageAstNodeHandle astNode,
                           const NamespaceString& nss)
            // NOTE: There is no original BSON since this stage is created from an AST node
            // desugared without BSON. For now we create a dummy spec with the stage name, but it
            // will go unused since LiteParsedExpanded will not exist at the top-level
            // LiteParsedPipeline.
            : LiteParsedDocumentSource(BSON(stageName << BSONObj()).firstElement()),
              _astNode(std::move(astNode)),
              _properties(_astNode->getProperties()),
              _nss(nss) {}

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<ExpandedStageParams>(_astNode->clone());
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            PrivilegeVector privileges;

            if (const auto& requiredPrivileges = _properties.getRequiredPrivileges()) {
                for (const auto& rp : *requiredPrivileges) {
                    tassert(
                        11350602,
                        "Only 'namespace' resourcePattern is supported for extension privileges",
                        rp.getResourcePattern() ==
                            MongoExtensionPrivilegeResourcePatternEnum::kNamespace);

                    ActionSet actions;
                    for (const auto& entry : rp.getActions()) {
                        actions.addAction(static_properties_util::toActionType(entry.getAction()));
                    }

                    tassert(11350600,
                            "requiredPrivileges.actions must not be empty.",
                            !actions.empty());
                    Privilege::addPrivilegeToPrivilegeVector(
                        &privileges, Privilege{ResourcePattern::forExactNamespace(_nss), actions});
                }
            }

            return privileges;
        }

        bool isInitialSource() const override {
            return !_properties.getRequiresInputDocSource();
        }

        bool requiresAuthzChecks() const override {
            // If the stage specifies a non-empty set of required privileges, mandatory auth checks
            // are required. Otherwise, it is safe to opt out of auth checks.
            const auto& properties = _properties.getRequiredPrivileges();
            return properties.has_value() && !properties->empty();
        }

        std::unique_ptr<LiteParsedDocumentSource> clone() const override {
            return std::make_unique<LiteParsedExpanded>(
                getParseTimeName(), _astNode->clone(), _nss);
        }

        bool isExtensionVectorSearchStage() const override;

        ViewPolicy getViewPolicy() const override;

    private:
        const AggStageAstNodeHandle _astNode;
        const MongoExtensionStaticProperties _properties;
        const NamespaceString _nss;
    };

    // Construction of a source or transform stage that expanded from a desugar stage. This stage
    // does not hold a parse node and therefore has no concept of a query shape. Its shape
    // responsibility comes from the desugar stage it expanded from.
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, AggStageAstNodeHandle astNode) {
        return boost::intrusive_ptr<DocumentSourceExtensionOptimizable>(
            new DocumentSourceExtensionOptimizable(expCtx, std::move(astNode)));
    }

    /**
     * Construct directly from a parse node handle.
     *
     * NOTE: This should only be used when the parse node handle expands into a *single
     * extension-allocated AST node* (e.g. when parsing on a shard after the router has already
     * expanded and serialized the parse node).
     */
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggStageParseNodeHandle& parseNodeHandle);

    /**
     * Construct a DocumentSourceExtensionOptimizable from a logical stage handle.
     *
     * Note: it is important that the input properties match the logical stage type being passed in.
     * Therefore this should only be used when "cloning" an existing document source - e.g. for
     * creating DocumentSources from DPL logical stage handles.
     */
    static boost::intrusive_ptr<DocumentSourceExtensionOptimizable> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        LogicalAggStageHandle logicalStage,
        const MongoExtensionStaticProperties& properties) {
        return boost::intrusive_ptr<DocumentSourceExtensionOptimizable>(
            new DocumentSourceExtensionOptimizable(expCtx, std::move(logicalStage), properties));
    }

    /**
     * Recursively expands a parse node handle into a list of DocumentSources. Used during expansion
     * of extension-allocated parse nodes returned from getDistributedPlanLogic().
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> expandParseNode(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const AggStageParseNodeHandle& parseNodeHandle);

    // This method is invoked by extensions to register descriptor.
    static void registerStage(AggStageDescriptorHandle descriptor);

    const char* getSourceName() const override {
        return _stageName.c_str();
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    Value serialize(const SerializationOptions& opts) const override;

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    static const Id& id;

    Id getId() const override;

    const MongoExtensionStaticProperties& getStaticProperties() const {
        return _properties;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override;

    // Wrapper around the LogicalAggStageHandle::compile() method. Returns an ExecAggStageHandle.
    ExecAggStageHandle compile() {
        return _logicalStage->compile();
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override;

protected:
    /**
     * NB : Here we keep a copy of the stage name to service getSourceName().
     * It is tempting to rely on the name which is provided by the _staticDescriptor, however, that
     * is risky because we need to return a const char* in getSourceName() but a
     * MongoExtensionByteView coming from the extension is not guaranteed to contain a null
     * terminator.
     **/
    const std::string _stageName;
    const MongoExtensionStaticProperties _properties;
    const LogicalAggStageHandle _logicalStage;

    DocumentSourceExtensionOptimizable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       AggStageAstNodeHandle astNode)
        : DocumentSource(astNode->getName(), expCtx),
          _stageName(std::string(astNode->getName())),
          _properties(astNode->getProperties()),
          _logicalStage([&]() {
              tassert(11647800,
                      "DocumentSourceExtensionOptimizable received invalid expression context",
                      expCtx.get() != nullptr);
              auto catalogContext = CatalogContext(*expCtx);
              return astNode->bind(catalogContext.getAsBoundaryType());
          }()) {}

    DocumentSourceExtensionOptimizable(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       LogicalAggStageHandle logicalStage,
                                       const MongoExtensionStaticProperties& properties)
        : DocumentSource(logicalStage->getName(), expCtx),
          _stageName(std::string(logicalStage->getName())),
          _properties(properties),
          _logicalStage(std::move(logicalStage)) {}

private:
    // Do not support copy or move.
    DocumentSourceExtensionOptimizable(const DocumentSourceExtensionOptimizable&) = delete;
    DocumentSourceExtensionOptimizable(DocumentSourceExtensionOptimizable&&) = delete;
    DocumentSourceExtensionOptimizable& operator=(const DocumentSourceExtensionOptimizable&) =
        delete;
    DocumentSourceExtensionOptimizable& operator=(DocumentSourceExtensionOptimizable&&) = delete;
};

namespace helper {

template <typename OnParseHost, typename OnParseExt, typename OnAstHost, typename OnAstExt>
inline void visitExpandedNodes(std::vector<VariantNodeHandle>& expanded,
                               OnParseHost&& onParseHost,
                               OnParseExt&& onParseExt,
                               OnAstHost&& onAstHost,
                               OnAstExt&& onAstExt) {
    for (auto& node : expanded) {
        std::visit(
            [&](auto&& handle) {
                using H = std::decay_t<decltype(handle)>;
                // Case 1: Parse node handle.
                //   a) Host-allocated parse node: convert directly to a host
                //      DocumentSource using the host-provided BSON spec. No recursion
                //      in this branch.
                //   b) Extension-allocated parse node: Recurse on the parse node
                //      handle, splicing the results of its expansion.
                if constexpr (std::is_same_v<H, AggStageParseNodeHandle>) {
                    if (host::HostAggStageParseNode::isHostAllocated(*handle.get())) {
                        onParseHost(*static_cast<host::HostAggStageParseNode*>(handle.get()));
                    } else {
                        onParseExt(handle);
                    }
                }
                // Case 2: AST node handle.
                //   a) Host-allocated AST node: convert directly to a host DocumentSource using
                //      the host-provided BSON spec.
                //   b) Extension-allocated AST node: Construct a
                //      DocumentSourceExtensionOptimizable and release the AST node handle.
                else if constexpr (std::is_same_v<H, AggStageAstNodeHandle>) {
                    if (host::HostAggStageAstNode::isHostAllocated(*handle.get())) {
                        onAstHost(*static_cast<host::HostAggStageAstNode*>(handle.get()));
                    } else {
                        onAstExt(std::move(handle));
                    }
                }
            },
            node);
    }
}

}  // namespace helper

}  // namespace mongo::extension::host
