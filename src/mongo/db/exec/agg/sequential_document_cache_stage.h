// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/sequential_document_cache.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
class SequentialDocumentCacheStage final : public Stage {
public:
    using SequentialDocumentCachePtr = std::shared_ptr<SequentialDocumentCache>;
    SequentialDocumentCacheStage(std::string_view stageName,
                                 const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                 SequentialDocumentCachePtr cache);

protected:
    GetNextResult doGetNext() final;

private:
    SequentialDocumentCachePtr _cache;

    // This flag is set to prevent the cache stage from immediately serving from the cache after it
    // has exhausted input from its source for the first time.
    bool _cacheIsEOF = false;
};
}  // namespace mongo::exec::agg
