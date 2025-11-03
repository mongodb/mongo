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

#include "mongo/db/extension/host/document_source_extension.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host/document_source_extension_optimizable.h"
#include "mongo/db/extension/host/extension_stage.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/stage_descriptor.h"

namespace mongo::extension::host {

class DocumentSourceExtension::LiteParsedExpandable::ExpansionValidationFrame {
public:
    ExpansionValidationFrame(ExpansionState& state, std::string stageName)
        : _state(state), _stageName(std::move(stageName)) {
        const auto newDepth = _state.currDepth + 1;
        tassert(10955800,
                str::stream() << "Stage expansion exceeded maximum depth of " << kMaxExpansionDepth,
                newDepth <= kMaxExpansionDepth);

        const auto inserted = _state.seenStages.insert(_stageName).second;
        tassert(10955801,
                str::stream() << "Cycle detected during stage expansion for stage " << _stageName
                              << ": " << _formatCyclePath(_state, _stageName),
                inserted);

        _state.expansionPath.push_back(_stageName);
        _state.currDepth = newDepth;
    }

    ~ExpansionValidationFrame() noexcept {
        _state.seenStages.erase(_stageName);
        _state.expansionPath.pop_back();
        _state.currDepth--;
    }

    // Non-copyable, non-movable.
    ExpansionValidationFrame(const ExpansionValidationFrame&) = delete;
    ExpansionValidationFrame& operator=(const ExpansionValidationFrame&) = delete;

private:
    /**
     * Format the cyclical slice for an informative error message (e.g. A -> B -> A).
     */
    static std::string _formatCyclePath(const ExpansionState& state, const std::string& stageName) {
        size_t start = 0;
        // Find the position of the first occurrence of the cyclical stage.
        for (; start < state.expansionPath.size(); ++start) {
            if (state.expansionPath[start] == stageName) {
                break;
            }
        }

        StringBuilder sb;
        constexpr StringData arrow = " -> "_sd;
        // Construct the path starting from the first occurrence.
        for (size_t i = start; i < state.expansionPath.size(); ++i) {
            if (i > start) {
                sb << arrow;
            }
            sb << state.expansionPath[i];
        }
        // Close the loop with the cyclical stage.
        sb << arrow << stageName;
        return sb.str();
    }

    ExpansionState& _state;
    std::string _stageName;
};

LiteParsedList DocumentSourceExtension::LiteParsedExpandable::expand() {
    ExpansionState state;
    return expandImpl(_parseNode, state, _nss, _options);
}

LiteParsedList DocumentSourceExtension::LiteParsedExpandable::expandImpl(
    const AggStageParseNodeHandle& parseNodeHandle,
    ExpansionState& state,
    const NamespaceString& nss,
    const LiteParserOptions& options) {
    LiteParsedList outExpanded;
    auto expanded = parseNodeHandle.expand();

    for (auto& variantNodeHandle : expanded) {
        std::visit(
            [&](auto&& handle) {
                using H = std::decay_t<decltype(handle)>;

                // Case 1: Parse node handle
                //   a) Host-allocated parse node: convert directly to a host
                //      LiteParsedDocumentSource using the host-provided BSON spec. No recursion in
                //      this branch.
                //   b) Extension-allocated parse node: Enter a validation frame for expansion
                //      constraint enforcement (depth and cycles) and recurse on the parse node
                //      handle, splicing the results of its expansion.
                if constexpr (std::is_same_v<H, AggStageParseNodeHandle>) {
                    if (host::HostAggStageParseNode::isHostAllocated(*handle.get())) {
                        const auto& spec =
                            static_cast<host::HostAggStageParseNode*>(handle.get())->getBsonSpec();
                        outExpanded.emplace_back(
                            LiteParsedDocumentSource::parse(nss, spec, options));
                    } else {
                        const auto stageName = std::string(handle.getName());
                        ExpansionValidationFrame frame{state, stageName};
                        auto children = expandImpl(handle, state, nss, options);
                        outExpanded.splice(outExpanded.end(), children);
                    }
                }
                // Case 2: AST node handle. Wrap in LiteParsedExpanded and append directly to the
                // expanded result.
                else if constexpr (std::is_same_v<H, AggStageAstNodeHandle>) {
                    outExpanded.emplace_back(
                        std::make_unique<DocumentSourceExtension::LiteParsedExpanded>(
                            std::string(handle.getName()), std::move(handle)));
                }
            },
            variantNodeHandle);
    }

    return outExpanded;
}

// static
void DocumentSourceExtension::registerStage(AggStageDescriptorHandle descriptor) {
    auto nameStringData = descriptor.getName();
    auto id = DocumentSource::allocateId(nameStringData);
    auto nameAsString = std::string(nameStringData);

    using LiteParseFn = std::function<std::unique_ptr<LiteParsedDocumentSource>(
        const NamespaceString&, const BSONElement&, const LiteParserOptions&)>;

    auto parser = [&]() -> LiteParseFn {
        return [descriptor](const NamespaceString& nss,
                            const BSONElement& spec,
                            const LiteParserOptions& opts) {
            return DocumentSourceExtension::LiteParsedExpandable::parse(
                descriptor, nss, spec, opts);
        };
    }();

    registerStage(nameAsString, id, descriptor);

    LiteParsedDocumentSource::registerParser(nameAsString,
                                             std::move(parser),
                                             AllowedWithApiStrict::kAlways,
                                             AllowedWithClientType::kAny);
}

// static
void DocumentSourceExtension::registerStage(const std::string& name,
                                            DocumentSource::Id id,
                                            AggStageDescriptorHandle descriptor) {
    // Register the correct DocumentSource to construct the stage with.
    DocumentSource::registerParser(
        name,
        [descriptor](BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx)
            -> boost::intrusive_ptr<DocumentSource> {
            return boost::intrusive_ptr(new DocumentSourceExtensionOptimizable(
                specElem.fieldNameStringData(), expCtx, specElem.wrap(), descriptor));
        });

    // Register the correct exec::agg to execute the stage with.
    exec::agg::registerDocumentSourceToStageFn(
        id, [](const boost::intrusive_ptr<DocumentSource>& source) {
            auto* documentSource = dynamic_cast<DocumentSourceExtension*>(source.get());

            tassert(10980400, "expected 'DocumentSourceExtension' type", documentSource);

            return make_intrusive<exec::agg::ExtensionStage>(documentSource->getSourceName(),
                                                             documentSource->getExpCtx());
        });

    // Add the allocated id to the static map for lookup upon object construction.
    stageToIdMap[name] = id;
}

void DocumentSourceExtension::unregisterParser_forTest(const std::string& name) {
    DocumentSource::unregisterParser_forTest(name);
}

DocumentSourceExtension::DocumentSourceExtension(
    StringData name,
    const boost::intrusive_ptr<ExpressionContext>& exprCtx,
    BSONObj rawStage,
    AggStageDescriptorHandle staticDescriptor)
    : DocumentSource(name, exprCtx),
      _stageName(std::string(name)),
      _id(findStageId(std::string(name))),
      _parseNode(staticDescriptor.parse(rawStage)) {}

const char* DocumentSourceExtension::getSourceName() const {
    return _stageName.c_str();
}

DocumentSource::Id DocumentSourceExtension::getId() const {
    return _id;
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceExtension::distributedPlanLogic() {
    return boost::none;
}

StageConstraints DocumentSourceExtension::constraints(PipelineSplitState pipeState) const {
    auto constraints = StageConstraints(StageConstraints::StreamType::kStreaming,
                                        StageConstraints::PositionRequirement::kNone,
                                        StageConstraints::HostTypeRequirement::kNone,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kNotAllowed,
                                        UnionRequirement::kNotAllowed,
                                        ChangeStreamRequirement::kDenylist);
    return constraints;
}

DocumentSourceExtension::Id DocumentSourceExtension::findStageId(std::string stageName) {
    auto it = stageToIdMap.find(stageName);
    tassert(11250700,
            str::stream() << "Could not find id associated with extension stage " << stageName,
            it != stageToIdMap.end());

    return it->second;
}

DocumentSourceExtension::~DocumentSourceExtension() = default;

}  // namespace mongo::extension::host
