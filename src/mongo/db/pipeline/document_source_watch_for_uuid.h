/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
 * In the event that a single-collection $changeStream is opened on a namespace whose UUID is not
 * known, this stage will be added to the pipeline on both mongoD and mongoS. When the first event
 * is observed by the pipeline, DSWatchForUUID will extract the collection's UUID from the event's
 * resume token, and will use it to populate the pipeline's ExpressionContext::uuid.
 */
class DocumentSourceWatchForUUID final : public DocumentSource {
public:
    GetNextResult getNext() final;

    const char* getSourceName() const final {
        return "$watchForUUID";
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kNotAllowed,
                ChangeStreamRequirement::kChangeStreamStage};
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // This stage is created by the DocumentSourceChangeStream stage, so serializing it
        // here would result in it being created twice.
        return Value();
    }

    static boost::intrusive_ptr<DocumentSourceWatchForUUID> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        // Only created for a single-collection stream where the UUID does not exist.
        invariant(expCtx->isSingleNamespaceAggregation());
        invariant(!expCtx->uuid);
        return new DocumentSourceWatchForUUID(expCtx);
    }

private:
    /**
     * The static 'create' method must be used to build a DocumentSourceWatchForUUID.
     */
    DocumentSourceWatchForUUID(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(expCtx) {}
};

}  // namespace mongo
