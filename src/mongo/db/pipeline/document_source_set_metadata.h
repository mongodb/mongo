// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * SetMetadataTransformation applies a transformation that sets metadata on the document without
 * modifying any of the document fields. This is the core transformation logic for the $setMetadata
 * stage.
 */
class SetMetadataTransformation final : public TransformerInterface {
public:
    SetMetadataTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              boost::intrusive_ptr<Expression> metadataExpression,
                              DocumentMetadataFields::MetaType metaType);

    TransformerType getType() const final {
        return TransformerType::kSetMetadata;
    }

    DocumentMetadataFields::MetaType getMetaType() {
        return _metaType;
    }

    Document applyTransformation(const Document& input, const EvaluationContext& ctx) const final;

    void optimize() final;

    Document serializeTransformation(
        const query_shape::SerializationOptions& options = {}) const final;

    DepsTracker::State addDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    void describeTransformation(
        document_transformation::DocumentOperationVisitor& visitor) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    const boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _metadataExpression;
    const DocumentMetadataFields::MetaType _metaType;
};

DEFINE_LITE_PARSED_STAGE_INTERNAL_DERIVED(SetMetadata);

/**
 * $setMetadata takes one {<$meta field> : <Expression>} pair and sets the metadata on each incoming
 * document with the result of evaluating that expression.
 *
 * This is implemented as an extension of DocumentSourceSingleDocumentTransformation.
 */
class DocumentSourceSetMetadata final {
public:
    static constexpr std::string_view kStageName = "$setMetadata"sv;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<Expression> metadataExpression,
        DocumentMetadataFields::MetaType metaType);
};
}  // namespace mongo
