/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"

namespace mongo {

/**
 * Part of the change stream API machinery used to look up the post-image of a document. Uses
 * the "documentKey" field of the input to look up the new version of the document.
 *
 * Uses the ExpressionContext to determine what collection to look up into.
 * TODO SERVER-29134 When we allow change streams on multiple collections, this will need to change.
 */
class DocumentSourceLookupChangePostImage final : public DocumentSourceNeedsMongoProcessInterface {
public:
    static constexpr StringData kStageName = "$_internalLookupChangePostImage"_sd;
    static constexpr StringData kFullDocumentFieldName =
        DocumentSourceChangeStream::kFullDocumentField;

    /**
     * Creates a DocumentSourceLookupChangePostImage stage.
     */
    static boost::intrusive_ptr<DocumentSourceLookupChangePostImage> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceLookupChangePostImage(expCtx);
    }

    /**
     * Only modifies a single path: "fullDocument".
     */
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, {kFullDocumentFieldName.toString()}, {}};
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        invariant(pipeState != Pipeline::SplitState::kSplitForShards);
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     pipeState == Pipeline::SplitState::kUnsplit
                                         ? HostTypeRequirement::kNone
                                         : HostTypeRequirement::kMongoS,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);

        constraints.canSwapWithMatch = true;
        return constraints;
    }

    GetDepsReturn getDependencies(DepsTracker* deps) const {
        // The namespace is not technically needed yet, but we will if there is more than one
        // collection involved.
        deps->fields.insert(DocumentSourceChangeStream::kNamespaceField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kDocumentKeyField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kOperationTypeField.toString());
        deps->fields.insert(DocumentSourceChangeStream::kIdField.toString());
        // This stage does not restrict the output fields to a finite set, and has no impact on
        // whether metadata is available or needed.
        return SEE_NEXT;
    }

    /**
     * Performs the lookup to retrieve the full document.
     */
    GetNextResult getNext() final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        if (explain) {
            return Value{Document{{kStageName, Document()}}};
        }
        return Value();  // Do not serialize this stage unless we're explaining.
    }

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

private:
    DocumentSourceLookupChangePostImage(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceNeedsMongoProcessInterface(expCtx) {}

    /**
     * Uses the "documentKey" field from 'updateOp' to look up the current version of the document.
     * Returns Value(BSONNULL) if the document couldn't be found.
     */
    Value lookupPostImage(const Document& updateOp) const;

    /**
     * Throws a AssertionException if the namespace found in 'inputDoc' doesn't match the one on the
     * ExpressionContext.
     */
    NamespaceString assertNamespaceMatches(const Document& inputDoc) const;
};

}  // namespace mongo
