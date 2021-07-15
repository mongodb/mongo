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

#include "mongo/db/pipeline/document_source.h"

namespace mongo {

/**
 * This stage will take a list of oplog entries as input and forge a no-op pre- or post-image to be
 * returned before each 'findAndModify' oplog entry that has the 'needsRetryImage' field. This stage
 * also downconverts 'findAndModify' entries by stripping the 'needsRetryImage' field and appending
 * the appropriate 'preImageOpTime' or 'postImageOpTime' field.
 */
class DocumentSourceFindAndModifyImageLookup : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalFindAndModifyImageLookup"_sd;

    static boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup> create(
        const boost::intrusive_ptr<ExpressionContext>&);

    static boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup> createFromBson(
        const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const {
        return DocumentSourceFindAndModifyImageLookup::kStageName.rawData();
    }

protected:
    DocumentSource::GetNextResult doGetNext() override;

private:
    DocumentSourceFindAndModifyImageLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx);

    // Forges the no-op pre- or post-image document to be returned. Also downconverts the original
    // 'findAndModify' oplog entry and stashes it.
    boost::optional<Document> _forgeNoopImageDoc(Document inputDoc, OperationContext* opCtx);

    // Represents the stashed 'findAndModify' document. This indicates that the previous document
    // emitted was a forged pre- or post-image.
    boost::optional<Document> _stashedFindAndModifyDoc;
};

}  // namespace mongo
