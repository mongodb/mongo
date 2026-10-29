// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream check invalidate aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamCheckInvalidate, which handles the optimization part.
 */
class ChangeStreamCheckInvalidateStage final : public Stage {
public:
    ChangeStreamCheckInvalidateStage(std::string_view stageName,
                                     const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     boost::optional<ResumeTokenData> startAfterInvalidate);

private:
    GetNextResult doGetNext() final;

    /**
     * Classifies how an invalidation events (e.g. "drop", "dropDatabase", "rename") will be
     * handled.
     */
    enum class ClassificationType {
        // Generate an "invalidate" and buffer it as the next to-be-returned event. Still return the
        // current event.
        kGenerateInvalidateEvent,

        // Rethrow the invalidation event as a 'ChangeStreamStartAfterInvalidateInfo' exception.
        // The 'ChangeStreamEnsureResumeTokenPresent' stage will catch this and handle it.
        kRethrowForResumeTokenVerification,

        // Return the incoming invalidation event as is.
        kSwallow,
    };

    ClassificationType _classifyInvalidationForStartAfter(
        const ResumeTokenData& resumeTokenData) const;

    /**
     * Build an "invalidate" change event from an invalidation event (e.g. "drop", "dropDatabase",
     * "rename").
     */
    Document _buildInvalidateEvent(const ResumeTokenData& resumeTokenData,
                                   const Document& doc) const;

    boost::optional<ResumeTokenData> _startAfterInvalidate;

    boost::optional<Document> _queuedInvalidate;
    boost::optional<ChangeStreamInvalidationInfo> _queuedException;
};
}  // namespace mongo::exec::agg
