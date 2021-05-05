/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"

namespace mongo {

/**
 * Part of the change stream API machinery used to look up the pre-image of a document.
 *
 * After a document that should have its pre-image included is transformed from the oplog,
 * its "fullDocumentBeforeChange" field shall be the optime of the noop oplog entry containing the
 * pre-image. This stage replaces that field with the actual pre-image document.
 */
class DocumentSourceLookupChangePreImage final : public DocumentSource,
                                                 public ChangeStreamStageSerializationInterface {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamLookupPreImage"_sd;
    static constexpr StringData kFullDocumentBeforeChangeFieldName =
        DocumentSourceChangeStream::kFullDocumentBeforeChangeField;

    /**
     * Creates a DocumentSourceLookupChangePostImage stage.
     */
    static boost::intrusive_ptr<DocumentSourceLookupChangePreImage> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static boost::intrusive_ptr<DocumentSourceLookupChangePreImage> createFromBson(
        const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceLookupChangePreImage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       FullDocumentBeforeChangeModeEnum mode)
        : DocumentSource(kStageName, expCtx), _fullDocumentBeforeChangeMode(mode) {
        // This stage should never be created with FullDocumentBeforeChangeMode::kOff.
        invariant(_fullDocumentBeforeChangeMode != FullDocumentBeforeChangeModeEnum::kOff);
    }

    /**
     * Only modifies a single path: "fullDocumentBeforeChange".
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet,
                {kFullDocumentBeforeChangeFieldName.toString()},
                {}};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        invariant(pipeState != Pipeline::SplitState::kSplitForShards);
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
        return constraints;
    }

    // Conceptually, this stage must always run on the shards part of the pipeline. At
    // present, however, pre-image retrieval is not supported in a sharded cluster, and
    // so the distributed plan logic will not be used.
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return DistributedPlanLogic{this, nullptr, boost::none};
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const {
        deps->fields.insert(DocumentSourceChangeStream::kFullDocumentBeforeChangeField.toString());
        // This stage does not restrict the output fields to a finite set, and has no impact on
        // whether metadata is available or needed.
        return DepsTracker::State::SEE_NEXT;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        return ChangeStreamStageSerializationInterface::serializeToValue(explain);
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

private:
    /**
     * Performs the lookup to retrieve the full pre-image document for applicable operations.
     */
    GetNextResult doGetNext() final;

    /**
     * Looks up and returns a pre-image document at the specified opTime in the oplog. Returns
     * boost::none if the mode is "kWhenAvailable" and no such oplog entry was found. Throws if the
     * pre-image mode is "kRequired" and no entry was found. Invariants that if an oplog entry with
     * the given opTime is found, it is a no-op entry with a valid non-empty pre-image document.
     */
    boost::optional<Document> lookupPreImage(const Document& inputDoc,
                                             const repl::OpTime& opTime) const;

    Value serializeLegacy(boost::optional<ExplainOptions::Verbosity> explain) const final;
    Value serializeLatest(boost::optional<ExplainOptions::Verbosity> explain) const final;

    // Determines whether pre-images are strictly required or may be included only when available.
    FullDocumentBeforeChangeModeEnum _fullDocumentBeforeChangeMode =
        FullDocumentBeforeChangeModeEnum::kOff;
};

}  // namespace mongo
