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
#include "mongo/db/extension/shared/handle/aggregation_stage/parse_node.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"
#include "mongo/db/pipeline/desugarer.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

namespace mongo::extension {

class DocumentSourceExtensionTest;

namespace host {
using LiteParsedList = std::list<std::unique_ptr<LiteParsedDocumentSource>>;

class LoadExtensionsTest;
class LoadNativeVectorSearchTest;

/**
 * A DocumentSource implementation for an extension aggregation stage. DocumentSourceExtension is a
 * facade around handles to extension API objects.
 */
class DocumentSourceExtension : public DocumentSource {
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
            auto parseNode = descriptor.parse(spec.wrap());
            return std::make_unique<LiteParsedExpandable>(
                spec.fieldName(), std::move(parseNode), nss, options);
        }

        LiteParsedExpandable(std::string stageName,
                             AggStageParseNodeHandle parseNode,
                             const NamespaceString& nss,
                             const LiteParserOptions& options)
            : LiteParsedDocumentSource(std::move(stageName)),
              _parseNode(std::move(parseNode)),
              _nss(nss),
              _options(options),
              _expanded([&] {
                  auto expanded = expand();
                  tassert(10905600,
                          "LiteParsedExpandable must not have an empty expanded pipeline",
                          !expanded.empty());
                  return expanded;
              }()) {}

        /**
         * Return the pre-computed expanded pipeline.
         */
        const LiteParsedList& getExpandedPipeline() const {
            return _expanded;
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
        const LiteParsedList _expanded;
    };

    /**
     * A LiteParsedDocumentSource implementation for extension stages mapping to an
     * AggStageAstNode.
     */
    class LiteParsedExpanded : public LiteParsedDocumentSource {
    public:
        LiteParsedExpanded(std::string stageName,
                           AggStageAstNodeHandle astNode,
                           const NamespaceString& nss)
            : LiteParsedDocumentSource(std::move(stageName)),
              _astNode(std::move(astNode)),
              _properties(_astNode.getProperties()),
              _nss(nss) {}

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
                        actions.addAction(toActionType(entry.getAction()));
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

    private:
        static ActionType toActionType(const MongoExtensionPrivilegeActionEnum& action) {
            switch (action) {
                case MongoExtensionPrivilegeActionEnum::kFind:
                    return ActionType::find;
                case MongoExtensionPrivilegeActionEnum::kListIndexes:
                    return ActionType::listIndexes;
                case MongoExtensionPrivilegeActionEnum::kListSearchIndexes:
                    return ActionType::listSearchIndexes;
                case MongoExtensionPrivilegeActionEnum::kPlanCacheRead:
                    return ActionType::planCacheRead;
                case MongoExtensionPrivilegeActionEnum::kCollStats:
                    return ActionType::collStats;
                case MongoExtensionPrivilegeActionEnum::kIndexStats:
                    return ActionType::indexStats;
                default:
                    MONGO_UNREACHABLE_TASSERT(11350601);
            }
        }

        const AggStageAstNodeHandle _astNode;
        const MongoExtensionStaticProperties _properties;
        const NamespaceString _nss;
    };

    const char* getSourceName() const override;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override;

    void addVariableRefs(std::set<Variables::Id>* refs) const override {}

    StageConstraints constraints(PipelineSplitState pipeState) const override;

    Value serialize(const SerializationOptions& opts) const override = 0;

    // This method is invoked by extensions to register descriptor.
    static void registerStage(AggStageDescriptorHandle descriptor);

    // Declare DocumentSourceExtension to be pure virtual.
    ~DocumentSourceExtension() override = 0;

private:
    /**
     * Give access to DocumentSourceExtensionTest/LoadExtensionsTest to unregister parser.
     * unregisterParser_forTest is only meant to be used in the context of unit
     * tests. This is because the parserMap is not thread safe, so modifying it at runtime is
     * unsafe.
     */
    friend class mongo::extension::DocumentSourceExtensionTest;
    friend class mongo::extension::host::LoadExtensionsTest;
    friend class mongo::extension::host::LoadNativeVectorSearchTest;
    static void unregisterParser_forTest(const std::string& name);

protected:
    DocumentSourceExtension(StringData name,
                            const boost::intrusive_ptr<ExpressionContext>& exprCtx);

    /**
     * NB : Here we keep a copy of the stage name to service getSourceName().
     * It is tempting to rely on the name which is provided by the _staticDescriptor, however, that
     * is risky because we need to return a const char* in getSourceName() but a
     * MongoExtensionByteView coming from the extension is not guaranteed to contain a null
     * terminator.
     **/
    const std::string _stageName;

private:
    // Do not support copy or move.
    DocumentSourceExtension(const DocumentSourceExtension&) = delete;
    DocumentSourceExtension(DocumentSourceExtension&&) = delete;
    DocumentSourceExtension& operator=(const DocumentSourceExtension&) = delete;
    DocumentSourceExtension& operator=(DocumentSourceExtension&&) = delete;
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

}  // namespace host

}  // namespace mongo::extension
