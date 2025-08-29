/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"

namespace mongo {

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

    Document applyTransformation(const Document& input) const final;

    void optimize() final;

    Document serializeTransformation(const SerializationOptions& options = {}) const final;

    DepsTracker::State addDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final;

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    const boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _metadataExpression;
    const DocumentMetadataFields::MetaType _metaType;
};

/**
 * $setMetadata takes one {<$meta field> : <Expression>} pair and sets the metadata on each incoming
 * document with the result of evaluating that expression.
 *
 * This is implemented as an extension of DocumentSourceSingleDocumentTransformation.
 */
class DocumentSourceSetMetadata final {
public:
    static constexpr StringData kStageName = "$setMetadata"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::intrusive_ptr<Expression> metadataExpression,
        DocumentMetadataFields::MetaType metaType);
};
}  // namespace mongo
