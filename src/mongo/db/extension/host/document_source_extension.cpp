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
#include "mongo/db/extension/host/document_source_extension_expandable.h"
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

    helper::visitExpandedNodes(
        expanded,
        [&](const HostAggStageParseNode& hostParse) {
            const auto& spec = hostParse.getBsonSpec();
            outExpanded.emplace_back(LiteParsedDocumentSource::parse(nss, spec, options));
        },
        [&](const AggStageParseNodeHandle& handle) {
            const auto stageName = std::string(handle.getName());
            ExpansionValidationFrame frame{state, stageName};
            auto children = expandImpl(handle, state, nss, options);
            outExpanded.splice(outExpanded.end(), children);
        },
        [&](const HostAggStageAstNode& hostAst) {
            const auto& spec = hostAst.getIdLookupSpec();
            outExpanded.emplace_back(LiteParsedDocumentSource::parse(nss, spec, options));
        },
        [&](AggStageAstNodeHandle handle) {
            outExpanded.emplace_back(std::make_unique<LiteParsedExpanded>(
                std::string(handle.getName()), std::move(handle), nss));
        });

    return outExpanded;
}

// static
void DocumentSourceExtension::registerStage(AggStageDescriptorHandle descriptor) {
    auto nameStringData = descriptor.getName();
    auto stageName = std::string(nameStringData);

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

    // Register the correct DocumentSource to construct the stage with.
    DocumentSource::registerParser(
        stageName,
        [descriptor](BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx)
            -> boost::intrusive_ptr<DocumentSource> {
            return DocumentSourceExtensionExpandable::create(expCtx, specElem.wrap(), descriptor);
        });

    LiteParsedDocumentSource::registerParser(
        stageName, std::move(parser), AllowedWithApiStrict::kAlways, AllowedWithClientType::kAny);
}

void DocumentSourceExtension::unregisterParser_forTest(const std::string& name) {
    DocumentSource::unregisterParser_forTest(name);
}

DocumentSourceExtension::DocumentSourceExtension(
    StringData name, const boost::intrusive_ptr<ExpressionContext>& exprCtx)
    : DocumentSource(name, exprCtx), _stageName(std::string(name)) {}

const char* DocumentSourceExtension::getSourceName() const {
    return _stageName.c_str();
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceExtension::distributedPlanLogic() {
    return boost::none;
}

StageConstraints DocumentSourceExtension::constraints(PipelineSplitState pipeState) const {
    // Default constraints for extension stages.
    //
    // Only DocumentSourceExtensionOptimizable has access to MongoExtensionStaticProperties and
    // overrides constraints() accordingly. DocumentSourceExtensionExpandable is a pre-desugar
    // wrapper around an AggStageParseNode and therefore always uses these defaults.
    //
    // This is acceptable because the aggregate command calls validateCommon() twice:
    //   (1) pre-desugar, when Expandable stages are still present, and
    //   (2) post-desugar/optimization, when all extension stages have been replaced by their
    //       expanded children, whose own constraints() reflect the true placement/host semantics.
    //
    // As long as validateCommon() is run again after desugaring, these defaults should remain as
    // lenient as possible to avoid prematurely rejecting a valid pipeline. If new callers begin
    // relying on constraints() before desugaring for correctness, we may need to surface
    // constraint metadata on the ParseNode or delay constraint checks until after desugar.
    auto constraints = StageConstraints(StreamType::kStreaming,
                                        PositionRequirement::kNone,
                                        HostTypeRequirement::kNone,
                                        DiskUseRequirement::kNoDiskUse,
                                        FacetRequirement::kNotAllowed,
                                        TransactionRequirement::kNotAllowed,
                                        LookupRequirement::kNotAllowed,
                                        UnionRequirement::kNotAllowed,
                                        ChangeStreamRequirement::kDenylist);
    constraints.canRunOnTimeseries = false;

    return constraints;
}

DocumentSourceExtension::~DocumentSourceExtension() = default;

}  // namespace mongo::extension::host
