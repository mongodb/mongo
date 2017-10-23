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

#include "mongo/db/matcher/expression_parser.h"

#include <regex>

namespace mongo {

namespace {

/**
 * Finds the top-level field that 'expr' is over. Returns boost::none if the expression does not
 * have a top-level field name, or a non-OK status if there are multiple top-level field names.
 */
StatusWith<boost::optional<StringData>> parseTopLevelFieldName(MatchExpression* expr) {
    if (auto pathExpr = dynamic_cast<PathMatchExpression*>(expr)) {
        auto firstDotPos = pathExpr->path().find('.');
        if (firstDotPos == std::string::npos) {
            return {pathExpr->path()};
        }
        return {pathExpr->path().substr(0, firstDotPos)};
    } else if (expr->getCategory() == MatchExpression::MatchCategory::kLogical) {
        boost::optional<StringData> placeholder;
        for (size_t i = 0; i < expr->numChildren(); ++i) {
            auto statusWithId = parseTopLevelFieldName(expr->getChild(i));
            if (!statusWithId.isOK()) {
                return statusWithId.getStatus();
            }

            if (!placeholder) {
                placeholder = statusWithId.getValue();
                continue;
            }

            if (statusWithId.getValue() && placeholder != statusWithId.getValue()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Expected a single top-level field name, found '"
                                            << *placeholder
                                            << "' and '"
                                            << *statusWithId.getValue()
                                            << "'");
            }
        }
        return placeholder;
    }

    return {boost::none};
}

}  // namespace

bool ExpressionWithPlaceholder::equivalent(const ExpressionWithPlaceholder* other) const {
    if (!other) {
        return false;
    }
    return _placeholder == other->_placeholder && _filter->equivalent(other->_filter.get());
}

// The placeholder must begin with a lowercase letter and contain no special characters.
const std::regex ExpressionWithPlaceholder::placeholderRegex("^[a-z][a-zA-Z0-9]*$");

// static
StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> ExpressionWithPlaceholder::make(
    std::unique_ptr<MatchExpression> filter) {
    auto statusWithId = parseTopLevelFieldName(filter.get());
    if (!statusWithId.isOK()) {
        return statusWithId.getStatus();
    }

    boost::optional<std::string> placeholder;
    if (statusWithId.getValue()) {
        placeholder = statusWithId.getValue()->toString();
        if (!std::regex_match(*placeholder, placeholderRegex)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The top-level field name must be an alphanumeric "
                                           "string beginning with a lowercase letter, found '"
                                        << *placeholder
                                        << "'");
        }
    }

    auto exprWithPlaceholder =
        stdx::make_unique<ExpressionWithPlaceholder>(std::move(placeholder), std::move(filter));
    return {std::move(exprWithPlaceholder)};
}

void ExpressionWithPlaceholder::optimizeFilter() {
    _filter = MatchExpression::optimize(std::move(_filter));

    auto newPlaceholder = parseTopLevelFieldName(_filter.get());
    invariantOK(newPlaceholder.getStatus());

    if (newPlaceholder.getValue()) {
        _placeholder = newPlaceholder.getValue()->toString();
        dassert(std::regex_match(*_placeholder, placeholderRegex));
    } else {
        _placeholder = boost::none;
    }
}

}  // namespace mongo
