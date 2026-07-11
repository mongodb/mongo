// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream add pre image aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamAddPreImage, which handles the optimization part.
 */
class ChangeStreamAddPreImageStage final : public Stage {
public:
    ChangeStreamAddPreImageStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        const FullDocumentBeforeChangeModeEnum& fullDocumentBeforeChangeMode);

    /**
     * Removes the internal fields from the event and returns the string representation of it.
     */
    static std::string makePreImageNotFoundErrorMsg(const Document& event);

    /**
     * Retrieves the pre-image document given the specified 'preImageId'. Returns boost::none if no
     * such pre-image is available.
     */
    static boost::optional<Document> lookupPreImage(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                                    const Document& preImageId);

private:
    /**
     * Performs the lookup to retrieve the full pre-image document for applicable operations.
     */
    GetNextResult doGetNext() final;

    /**
     * Determines whether pre-images are strictly required or may be included only when available.
     */
    FullDocumentBeforeChangeModeEnum _fullDocumentBeforeChangeMode =
        FullDocumentBeforeChangeModeEnum::kOff;
};
}  // namespace mongo::exec::agg
