// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream add post image aggregation stage and
 * is part of the execution pipeline. It computes the post-image from the pre-image plus the oplog
 * update modification for the 'fullDocument: "required"' / '"whenAvailable"' modes. The
 * 'fullDocument: "updateLookup"' path lives in ChangeStreamUpdateLookupStage. Its construction is
 * based on DocumentSourceChangeStreamAddPostImage, which handles the optimization part.
 */
class ChangeStreamAddPostImageStage final : public Stage {
public:
    ChangeStreamAddPostImageStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  const FullDocumentModeEnum& fullDocumentMode);

private:
    /**
     * Performs the lookup to retrieve the full document.
     */
    GetNextResult doGetNext() final;

    /**
     * Computes a post-image by taking a pre-image and applying an update modification that is
     * stored in the oplog entry. Returns boost::none if no pre-image information is available.
     */
    boost::optional<Document> generatePostImage(const Document& updateOp) const;

    /**
     * Determines whether post-images are strictly required or may be included only when available,
     * and whether to return a point-in-time post-image or the most current majority-committed
     * version of the updated document.
     */
    FullDocumentModeEnum _fullDocumentMode = FullDocumentModeEnum::kDefault;
};
}  // namespace mongo::exec::agg
