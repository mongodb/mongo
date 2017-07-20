/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_with_placeholder.h"

#include <regex>

#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"

namespace mongo {

namespace {

/**
* Finds the top-level field that 'expr' is over. This must be unique and not the empty string.
*/
StatusWith<StringData> parseTopLevelFieldName(MatchExpression* expr) {
    switch (expr->getCategory()) {
        case MatchExpression::MatchCategory::kLeaf:
        case MatchExpression::MatchCategory::kArrayMatching: {
            auto firstDotPos = expr->path().find('.');
            if (firstDotPos == std::string::npos) {
                return expr->path();
            }
            return expr->path().substr(0, firstDotPos);
        }
        case MatchExpression::MatchCategory::kLogical: {
            if (expr->numChildren() == 0) {
                return Status(ErrorCodes::FailedToParse, "No top-level field name found.");
            }

            StringData placeholder;
            for (size_t i = 0; i < expr->numChildren(); ++i) {
                auto statusWithId = parseTopLevelFieldName(expr->getChild(i));
                if (!statusWithId.isOK()) {
                    return statusWithId.getStatus();
                }

                if (placeholder == StringData()) {
                    placeholder = statusWithId.getValue();
                    continue;
                }

                if (placeholder != statusWithId.getValue()) {
                    return Status(
                        ErrorCodes::FailedToParse,
                        str::stream()
                            << "Each array filter must use a single top-level field name, found '"
                            << placeholder
                            << "' and '"
                            << statusWithId.getValue()
                            << "'");
                }
            }
            return placeholder;
        }
        case MatchExpression::MatchCategory::kOther: {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "Match expression does not support placeholders.");
        }
    }

    MONGO_UNREACHABLE;
}

}  // namespace

// The placeholder must begin with a lowercase letter and contain no special characters.
const std::regex ExpressionWithPlaceholder::placeholderRegex("^[a-z][a-zA-Z0-9]*$");

// static
StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> ExpressionWithPlaceholder::parse(
    BSONObj rawFilter, const CollatorInterface* collator) {
    StatusWithMatchExpression statusWithFilter =
        MatchExpressionParser::parse(rawFilter, ExtensionsCallbackDisallowExtensions(), collator);

    if (!statusWithFilter.isOK()) {
        return statusWithFilter.getStatus();
    }
    auto filter = std::move(statusWithFilter.getValue());

    auto statusWithId = parseTopLevelFieldName(filter.get());
    if (!statusWithId.isOK()) {
        return statusWithId.getStatus();
    }
    auto placeholder = statusWithId.getValue().toString();
    if (!std::regex_match(placeholder, placeholderRegex)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "The top-level field name must be an alphanumeric "
                                       "string beginning with a lowercase letter, found '"
                                    << placeholder
                                    << "'");
    }

    auto exprWithPlaceholder =
        stdx::make_unique<ExpressionWithPlaceholder>(std::move(placeholder), std::move(filter));
    return {std::move(exprWithPlaceholder)};
}

}  // namespace mongo
