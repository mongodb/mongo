// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
class FindAndModifyImageLookupStage final : public Stage {
public:
    FindAndModifyImageLookupStage(std::string_view stageName,
                                  const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                  bool includeCommitTransactionTimestamp);

private:
    GetNextResult doGetNext() final;

    // Downconverts 'findAndModify' or 'applyOps' entries with 'needsRetryImage'. If an oplog entry
    // document has 'needsRetryImage' set, we downconvert and stash the document as
    // '_stashedDownconvertedDoc' and then forge a no-op pre- or post-image document and return it.
    Document downConvertIfNeedsRetryImage(Document inputDoc);

    // Set to true if the input oplog entry documents can have a transaction commit timestamp
    // attached to it.
    const bool _includeCommitTransactionTimestamp;

    // Represents the stashed downconverted 'findAndModify' or 'applyOps' oplog entry document.
    // This indicates that the previous document emitted was a forged pre- or post-image.
    boost::optional<Document> _stashedDownconvertedDoc = boost::none;
};
}  // namespace mongo::exec::agg
