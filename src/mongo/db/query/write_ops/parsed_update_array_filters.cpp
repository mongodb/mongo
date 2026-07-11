// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/parsed_update_array_filters.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

StatusWith<std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>>>
parsedUpdateArrayFilters(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const std::vector<BSONObj>& rawArrayFiltersIn,
                         const NamespaceString& nss) {
    std::map<std::string_view, std::unique_ptr<ExpressionWithPlaceholder>> arrayFiltersOut;
    for (const auto& rawArrayFilter : rawArrayFiltersIn) {
        auto parsedArrayFilter =
            MatchExpressionParser::parse(rawArrayFilter,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);

        if (!parsedArrayFilter.isOK()) {
            return parsedArrayFilter.getStatus().withContext("Error parsing array filter");
        }
        auto parsedArrayFilterWithPlaceholder =
            ExpressionWithPlaceholder::make(std::move(parsedArrayFilter.getValue()));
        if (!parsedArrayFilterWithPlaceholder.isOK()) {
            return parsedArrayFilterWithPlaceholder.getStatus().withContext(
                "Error parsing array filter");
        }
        auto finalArrayFilter = std::move(parsedArrayFilterWithPlaceholder.getValue());
        auto fieldName = finalArrayFilter->getPlaceholder();
        if (!fieldName) {
            return Status(
                ErrorCodes::FailedToParse,
                "Cannot use an expression without a top-level field name in arrayFilters");
        }
        if (arrayFiltersOut.find(*fieldName) != arrayFiltersOut.end()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "Found multiple array filters with the same top-level field name "
                              << *fieldName);
        }

        arrayFiltersOut[*fieldName] = std::move(finalArrayFilter);
    }

    return std::move(arrayFiltersOut);
}

}  // namespace mongo
