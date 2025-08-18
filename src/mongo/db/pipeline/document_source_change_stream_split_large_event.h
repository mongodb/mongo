/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceChangeStreamSplitLargeEvent final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$changeStreamSplitLargeEvent"_sd;

    static boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    static boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSource::GetModPathsReturn getModifiedPaths() const final;

    // This stage does not reference any user or system variables.
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    void validatePipelinePosition(bool alreadyOptimized,
                                  DocumentSourceContainer::const_iterator pos,
                                  const DocumentSourceContainer& container) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

protected:
    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage>
    documentSourceChangeStreamSplitLargeEventToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // This constructor is private, callers should use the 'create()' method above.
    DocumentSourceChangeStreamSplitLargeEvent(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              boost::optional<ResumeTokenData> resumeAfterSplit);

    boost::optional<ResumeTokenData> _resumeAfterSplit;
};

}  // namespace mongo
