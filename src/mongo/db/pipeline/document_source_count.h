// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Count);

/**
 * The $count stage is an alias for a $group stage followed by a $project stage.
 */
class DocumentSourceCount final {
public:
    static constexpr std::string_view kStageName = "$count"sv;

    /**
     * Returns a $group stage followed by a $project stage.
     */
    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceCount directly, use createFromBson() instead.
    DocumentSourceCount() = default;
};

}  // namespace mongo
