/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/ops/parsed_update_array_filters.h"
#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

StatusWith<std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>>
parsedUpdateArrayFilters(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const std::vector<BSONObj>& rawArrayFiltersIn,
                         const NamespaceString& nss) {
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> arrayFiltersOut;
    for (auto rawArrayFilter : rawArrayFiltersIn) {
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
