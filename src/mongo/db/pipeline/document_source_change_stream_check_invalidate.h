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
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This stage is used internally for change stream notifications to artificially generate an
 * "invalidate" entry for commands that should invalidate the change stream (e.g. collection drop
 * for a single-collection change stream). It is not intended to be created by the user.
 */
class DocumentSourceChangeStreamCheckInvalidate final
    : public DocumentSourceInternalChangeStreamStage {
public:
    static constexpr StringData kStageName = "$_internalChangeStreamCheckInvalidate"_sd;

    const char* getSourceName() const final {
        // This is used in error reporting.
        return DocumentSourceChangeStreamCheckInvalidate::kStageName.data();
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kChangeStreamStage);
        constraints.consumesLogicalCollectionData = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    Value doSerialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckInvalidate> createFromBson(
        BSONElement spec, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSourceChangeStreamCheckInvalidate> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static const Id& id;

    Id getId() const override {
        return id;
    }

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamCheckInvalidateToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Use the create static method to create a DocumentSourceChangeStreamCheckInvalidate.
     */
    DocumentSourceChangeStreamCheckInvalidate(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              boost::optional<ResumeTokenData> startAfterInvalidate)
        : DocumentSourceInternalChangeStreamStage(kStageName, expCtx),
          _startAfterInvalidate(std::move(startAfterInvalidate)) {
        invariant(!_startAfterInvalidate ||
                  _startAfterInvalidate->fromInvalidate == ResumeTokenData::kFromInvalidate);
    }

    boost::optional<ResumeTokenData> _startAfterInvalidate;
};

}  // namespace mongo
