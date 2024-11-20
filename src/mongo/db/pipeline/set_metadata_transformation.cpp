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

#include "mongo/db/pipeline/set_metadata_transformation.h"

#include "mongo/base/error_codes.h"

namespace mongo {

using MetaType = DocumentMetadataFields::MetaType;

SetMetadataTransformation::SetMetadataTransformation(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::intrusive_ptr<Expression> metadataExpression,
    DocumentMetadataFields::MetaType metaType)
    : _expCtx(expCtx), _metadataExpression(std::move(metadataExpression)), _metaType(metaType) {
    tassert(9466800,
            "SetMetadataTransformation currently only supports MetaType::kScore.",
            metaType == MetaType::kScore);
}

Document SetMetadataTransformation::applyTransformation(const Document& input) const {
    // Compute new metadata.
    Value metaVal = _metadataExpression->evaluate(input, &_expCtx->variables);

    // The document must be mutable to modify metadata.
    MutableDocument newDoc(input);
    auto& metadata = newDoc.metadata();

    switch (_metaType) {
        case MetaType::kScore:
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "meta field 'score' requires numeric type, found "
                                  << typeName(metaVal.getType()),
                    metaVal.numeric());
            metadata.setScore(metaVal.getDouble());
            break;
        default:
            MONGO_UNREACHABLE_TASSERT(9466801);
            break;
    }

    return newDoc.freeze();
}

void SetMetadataTransformation::optimize() {
    _metadataExpression = _metadataExpression->optimize();
}

Document SetMetadataTransformation::serializeTransformation(
    boost::optional<ExplainOptions::Verbosity> explain, const SerializationOptions& options) const {
    MutableDocument output;

    // TODO SERVER-96793 Implement and test correct serialization.
    return Document{{"score"_sd, _metadataExpression->serialize(options)}};
}

DepsTracker::State SetMetadataTransformation::addDependencies(DepsTracker* deps) const {
    expression::addDependencies(_metadataExpression.get(), deps);

    // This transformation only adds metadata and does not modify fields, so later stages could need
    // access to either fields or metadata.
    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn SetMetadataTransformation::getModifiedPaths() const {
    // No field paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kFiniteSet, OrderedPathSet{}, {}};
}

Pipeline::SourceContainer::iterator SetMetadataTransformation::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    return std::next(itr);
}

void SetMetadataTransformation::addVariableRefs(std::set<Variables::Id>* refs) const {
    expression::addVariableRefs(_metadataExpression.get(), refs);
}

}  // namespace mongo
