// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_set_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"

namespace mongo {

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(setMetadata, SetMetadataLiteParsed::parse);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(setMetadata,
                                                   DocumentSourceSetMetadata,
                                                   SetMetadataStageParams);

using MetaType = DocumentMetadataFields::MetaType;

SetMetadataTransformation::SetMetadataTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression> metadataExpression,
    DocumentMetadataFields::MetaType metaType)
    : _expCtx(expCtx), _metadataExpression(std::move(metadataExpression)), _metaType(metaType) {}

Document SetMetadataTransformation::applyTransformation(const Document& input,
                                                        const EvaluationContext& ctx) const {
    // Compute new metadata.
    Value metaVal = _metadataExpression->evaluate(input, &_expCtx->variables, ctx);

    // The document must be mutable to modify metadata.
    MutableDocument newDoc(input);
    auto& metadata = newDoc.metadata();
    metadata.setMetaFieldFromValue(_metaType, metaVal);

    return newDoc.freeze();
}

void SetMetadataTransformation::optimize() {
    _metadataExpression = _metadataExpression->optimize();
}

Document SetMetadataTransformation::serializeTransformation(
    const query_shape::SerializationOptions& options) const {
    return Document{{DocumentMetadataFields::serializeMetaType(_metaType),
                     _metadataExpression->serialize(options)}};
}

DepsTracker::State SetMetadataTransformation::addDependencies(DepsTracker* deps) const {
    expression::addDependencies(_metadataExpression.get(), deps);

    // Mark that _metaType is available for downstream stages to access.
    deps->setMetadataAvailable(_metaType);

    // This transformation only sets metadata and does not modify fields, so later stages could need
    // access to either fields or metadata.
    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn SetMetadataTransformation::getModifiedPaths() const {
    // No field paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
}

void SetMetadataTransformation::describeTransformation(
    document_transformation::DocumentOperationVisitor& visitor) const {
    // No field paths are modified.
}

DocumentSourceContainer::iterator SetMetadataTransformation::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    return std::next(itr);
}

void SetMetadataTransformation::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_metadataExpression.get(), refs);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceSetMetadata::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    BSONObj obj = elem.embeddedObject();
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << kStageName
                << " only permits setting exactly one metadata field, but input specification has "
                << obj.nFields() << " fields",
            obj.nFields() == 1);

    BSONElement metaSpec = obj.firstElement();

    const auto metaType = DocumentMetadataFields::parseMetaType(metaSpec.fieldNameStringData());

    boost::intrusive_ptr<Expression> metadataExpression =
        Expression::parseOperand(expCtx.get(), std::move(metaSpec), expCtx->variablesParseState);

    return create(expCtx, std::move(metadataExpression), metaType);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceSetMetadata::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression> metadataExpression,
    DocumentMetadataFields::MetaType metaType) {
    const bool isIndependentOfAnyCollection = false;
    return make_intrusive<DocumentSourceSingleDocumentTransformation>(
        expCtx,
        std::make_unique<SetMetadataTransformation>(
            expCtx, std::move(metadataExpression), metaType),
        kStageName,
        isIndependentOfAnyCollection);
}
}  // namespace mongo
