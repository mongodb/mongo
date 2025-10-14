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
#include "mongo/db/extension/host/aggregation_stage/parse_node.h"
#include "mongo/db/extension/host_connector/handle/aggregation_stage/stage_descriptor.h"

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extension, DocumentSourceExtension::id);

std::list<std::unique_ptr<LiteParsedDocumentSource>>
DocumentSourceExtension::LiteParsedExpandable::expandImpl() {
    std::list<std::unique_ptr<LiteParsedDocumentSource>> outExpanded;
    auto expanded = _parseNode.expand();

    for (auto& variantNodeHandle : expanded) {
        std::visit(
            [&](auto&& handle) {
                using H = std::decay_t<decltype(handle)>;

                // Case 1: Parse node handle
                //   a) Host-allocated parse node: convert directly to a host
                //      LiteParsedDocumentSource using the host-provided BSON spec. No recursion in
                //      this branch.
                //   b) Extension-allocated parse node: Instantiate a LiteParsedExpandable and
                //      splice the results of its expansion recursively.
                if constexpr (std::is_same_v<H, host_connector::AggStageParseNodeHandle>) {
                    auto stageName = handle.getName();
                    if (host::HostAggStageParseNode::isHostAllocated(*handle.get())) {
                        const auto& spec =
                            static_cast<host::HostAggStageParseNode*>(handle.get())->getBsonSpec();
                        outExpanded.emplace_back(
                            LiteParsedDocumentSource::parse(_nss, spec, _options));
                    } else {
                        auto liteParsed =
                            std::make_unique<DocumentSourceExtension::LiteParsedExpandable>(
                                std::string(stageName), std::move(handle), _nss, _options);
                        auto children = liteParsed->expandImpl();
                        outExpanded.splice(outExpanded.end(), children);
                    }
                }
                // Case 2: AST node handle. Wrap in LiteParsedExpanded and append directly to the
                // expanded result.
                else if constexpr (std::is_same_v<H, host_connector::AggStageAstNodeHandle>) {
                    auto stageName = handle.getName();
                    outExpanded.emplace_back(
                        std::make_unique<DocumentSourceExtension::LiteParsedExpanded>(
                            std::string(stageName), std::move(handle)));
                }
            },
            variantNodeHandle);
    }

    return outExpanded;
}

// static
void DocumentSourceExtension::registerStage(host_connector::AggStageDescriptorHandle descriptor) {
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

    switch (descriptor.getType()) {
        case MongoExtensionAggStageType::kNoOp:
            registerStage(nameAsString, id, descriptor);
            break;
        default:
            tasserted(10596401,
                      str::stream()
                          << "Received unknown stage type while registering extension stage: "
                          << descriptor.getType());
    };

    LiteParsedDocumentSource::registerParser(nameAsString,
                                             std::move(parser),
                                             AllowedWithApiStrict::kAlways,
                                             AllowedWithClientType::kAny);
}

// static
void DocumentSourceExtension::registerStage(const std::string& name,
                                            DocumentSource::Id id,
                                            host_connector::AggStageDescriptorHandle descriptor) {
    DocumentSource::registerParser(
        name,
        [id, descriptor](BSONElement specElem,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
            -> boost::intrusive_ptr<DocumentSource> {
            return boost::intrusive_ptr(new DocumentSourceExtension(
                specElem.fieldNameStringData(), expCtx, id, specElem.wrap(), descriptor));
        });
}

void DocumentSourceExtension::unregisterParser_forTest(const std::string& name) {
    DocumentSource::unregisterParser_forTest(name);
}

DocumentSourceExtension::DocumentSourceExtension(
    StringData name,
    boost::intrusive_ptr<ExpressionContext> exprCtx,
    Id id,
    BSONObj rawStage,
    host_connector::AggStageDescriptorHandle staticDescriptor)
    : DocumentSource(name, exprCtx),
      _stageName(std::string(name)),
      _id(id),
      _raw_stage(rawStage.getOwned()),
      _staticDescriptor(staticDescriptor),
      _parseNode(staticDescriptor.parse(_raw_stage)) {}

const char* DocumentSourceExtension::getSourceName() const {
    return _stageName.c_str();
}

DocumentSource::Id DocumentSourceExtension::getId() const {
    return id;
}

Value DocumentSourceExtension::serialize(const SerializationOptions& opts) const {
    // TODO We need to call into the plugin here when we want to serialize for query shape, or
    // if optimizations change the shape of the stage definition.
    return Value(_raw_stage);
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

}  // namespace mongo::extension::host
