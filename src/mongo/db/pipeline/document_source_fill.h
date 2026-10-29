// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_fill_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Fill);

namespace document_source_fill {
using namespace std::literals::string_view_literals;
constexpr std::string_view kStageName = "$fill"sv;
constexpr std::string_view kLocfMethod = "locf"sv;
constexpr std::string_view kLinearInterpolateMethod = "linear"sv;

/*
 * '$fill' is sugar for '$setWindowFields' and $addFields'. This method delegates to the appropriate
 * parsers for those DocumentSources.
 */
std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

}  // namespace document_source_fill
}  // namespace mongo
