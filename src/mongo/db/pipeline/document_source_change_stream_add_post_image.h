/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <set>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Part of the change stream API machinery used to look up the post-image of a document. Uses the
 * "documentKey" field of the input to look up the new version of the document.
 */
class DocumentSourceChangeStreamAddPostImage final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamAddPostImage"_sd;
    static constexpr StringData kFullDocumentFieldName =
        DocumentSourceChangeStream::kFullDocumentField;
    static constexpr StringData kRawOplogUpdateSpecFieldName =
        DocumentSourceChangeStream::kRawOplogUpdateSpecField;
    static constexpr StringData kPreImageIdFieldName = DocumentSourceChangeStream::kPreImageIdField;
    static constexpr StringData kFullDocumentBeforeChangeFieldName =
        DocumentSourceChangeStream::kFullDocumentBeforeChangeField;

    /**
     * Creates a DocumentSourceChangeStreamAddPostImage stage.
     */
    static boost::intrusive_ptr<DocumentSourceChangeStreamAddPostImage> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec) {
        return new DocumentSourceChangeStreamAddPostImage(expCtx, spec.getFullDocument());
    }

    static boost::intrusive_ptr<DocumentSourceChangeStreamAddPostImage> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Only modifies: "fullDocument", "updateModification", "preImageId".
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet,
                {std::string{kFullDocumentFieldName},
                 std::string{kRawOplogUpdateSpecFieldName},
                 std::string{kPreImageIdFieldName}},
                {}};
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        invariant(pipeState != PipelineSplitState::kSplitForShards);

        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);

        constraints.canSwapWithMatch = true;
        constraints.consumesLogicalCollectionData = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const override {
        // The namespace is not technically needed yet, but we will if there is more than one
        // collection involved.
        deps->fields.insert(std::string{DocumentSourceChangeStream::kNamespaceField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kDocumentKeyField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kOperationTypeField});
        deps->fields.insert(std::string{DocumentSourceChangeStream::kIdField});

        // Fields needed for post-image computation.
        if (_fullDocumentMode != FullDocumentModeEnum::kUpdateLookup) {
            deps->fields.insert(
                std::string{DocumentSourceChangeStream::kFullDocumentBeforeChangeField});
            deps->fields.insert(std::string{DocumentSourceChangeStream::kRawOplogUpdateSpecField});
            deps->fields.insert(std::string{DocumentSourceChangeStream::kPreImageIdField});
        }

        // This stage does not restrict the output fields to a finite set, and has no impact on
        // whether metadata is available or needed.
        return DepsTracker::State::SEE_NEXT;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamAddPostImageToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    DocumentSourceChangeStreamAddPostImage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const FullDocumentModeEnum fullDocumentMode)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx),
          _fullDocumentMode(fullDocumentMode) {
        tassert(5842300,
                "the 'fullDocument' field cannot be 'default'",
                _fullDocumentMode != FullDocumentModeEnum::kDefault);
    }

    // Determines whether post-images are strictly required or may be included only when available,
    // and whether to return a point-in-time post-image or the most current majority-committed
    // version of the updated document.
    FullDocumentModeEnum _fullDocumentMode = FullDocumentModeEnum::kDefault;
};

}  // namespace mongo
