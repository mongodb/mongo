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

#include "mongo/db/pipeline/document_source_change_stream.h"

namespace mongo {
/**
 * A custom subclass of DocumentSourceMatch which is used to generate a $match stage to be applied
 * on the oplog. The stage requires itself to be the first stage in the pipeline.
 */
class DocumentSourceOplogMatch final : public DocumentSourceMatch,
                                       public ChangeStreamStageSerializationInterface {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamOplogMatch"_sd;

    DocumentSourceOplogMatch(BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMatch(std::move(filter), expCtx) {
        // A change stream pipeline should always create a tailable + awaitData cursor.
        expCtx->tailableMode = TailableModeEnum::kTailableAndAwaitData;
    }

    DocumentSourceOplogMatch(const DocumentSourceOplogMatch& other) : DocumentSourceMatch(other) {}

    virtual boost::intrusive_ptr<DocumentSourceMatch> clone() const {
        return make_intrusive<std::decay_t<decltype(*this)>>(*this);
    }

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);
    static boost::intrusive_ptr<DocumentSourceOplogMatch> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, bool showMigrationEvents);

    const char* getSourceName() const final;

    GetNextResult doGetNext() final {
        // We should never execute this stage directly. We expect this stage to be absorbed into the
        // cursor feeding the pipeline, and executing this stage may result in the use of the wrong
        // collation. The comparisons against the oplog must use the simple collation, regardless of
        // the collation on the ExpressionContext.
        MONGO_UNREACHABLE;
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain) const final {
        return ChangeStreamStageSerializationInterface::serializeToValue(explain);
    }

private:
    Value serializeLegacy(boost::optional<ExplainOptions::Verbosity> explain) const final;
    Value serializeLatest(boost::optional<ExplainOptions::Verbosity> explain) const final;
};
}  // namespace mongo
