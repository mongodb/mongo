/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
 * This stage is used internally for change stream notifications to artifically generate an
 * "invalidate" entry for commands that should invalidate the change stream (e.g. collection drop
 * for a single-collection change stream). It is not intended to be created by the user.
 */
class DocumentSourceCheckInvalidate final : public DocumentSource {
public:
    GetNextResult getNext() final;

    const char* getSourceName() const final {
        // This is used in error reporting.
        return "$_checkInvalidate";
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

    boost::optional<MergingLogic> mergingLogic() final {
        return boost::none;
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // This stage is created by the DocumentSourceChangeStream stage, so serializing it here
        // would result in it being created twice.
        return Value();
    }

    static boost::intrusive_ptr<DocumentSourceCheckInvalidate> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, bool ignoreFirstInvalidate) {
        return new DocumentSourceCheckInvalidate(expCtx, ignoreFirstInvalidate);
    }

private:
    /**
     * Use the create static method to create a DocumentSourceCheckInvalidate.
     */
    DocumentSourceCheckInvalidate(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  bool ignoreFirstInvalidate)
        : DocumentSource(expCtx), _ignoreFirstInvalidate(ignoreFirstInvalidate) {}

    boost::optional<Document> _queuedInvalidate;
    bool _ignoreFirstInvalidate;
};

}  // namespace mongo
