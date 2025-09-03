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

namespace mongo::extension::host {

ALLOCATE_DOCUMENT_SOURCE_ID(extension, DocumentSourceExtension::id);

stdx::unordered_set<NamespaceString> DocumentSourceExtension::LiteParsed::getInvolvedNamespaces()
    const {
    return stdx::unordered_set<NamespaceString>();
}

PrivilegeVector DocumentSourceExtension::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {
    // TODO SERVER-109056 Support getting required privileges from extensions.
    return {};
}

// static
void DocumentSourceExtension::registerStage(ExtensionAggregationStageDescriptorHandle descriptor) {
    auto nameStringData = descriptor.getName();
    auto id = DocumentSource::allocateId(nameStringData);
    auto nameAsString = std::string(nameStringData);

    switch (descriptor.getType()) {
        case MongoExtensionAggregationStageType::kNoOp:
            registerStage(nameAsString, id, descriptor);
            break;
        default:
            tasserted(10596401,
                      str::stream()
                          << "Received unknown stage type while registering extension stage: "
                          << descriptor.getType());
    };
    // TODO SERVER-109558 Call the correct version of DocumentSourceExtension::LiteParsed::parse or
    // the correct desugar parse function based on the descriptor type.
    // TODO SERVER-109558 We probably need to implement some kind of parser or registration macro
    // so that we can call the correct desugared stage parser, since there is no
    // DocumentSourceExtension::LiteParsedDesugar::parse.
    LiteParsedDocumentSource::registerParser(nameAsString,
                                             DocumentSourceExtension::LiteParsed::parse,
                                             AllowedWithApiStrict::kAlways,
                                             AllowedWithClientType::kAny);
}

// static
void DocumentSourceExtension::registerStage(
    const std::string& name,
    DocumentSource::Id id,
    extension::host::ExtensionAggregationStageDescriptorHandle descriptor) {
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
    extension::host::ExtensionAggregationStageDescriptorHandle staticDescriptor)
    : DocumentSource(name, exprCtx),
      _stageName(std::string(name)),
      _id(id),
      _raw_stage(rawStage.getOwned()),
      _staticDescriptor(staticDescriptor),
      _logicalStage(staticDescriptor.parse(_raw_stage)) {}

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
