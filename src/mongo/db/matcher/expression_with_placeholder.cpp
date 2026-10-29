// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_with_placeholder.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/util/pcre.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <new>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

bool matchesPlaceholderPattern(std::string_view placeholder) {
    // The placeholder must begin with a lowercase letter and contain no special characters.
    static StaticImmortal<pcre::Regex> kRe("^[[:lower:]][[:alnum:]]*$");
    return !!kRe->matchView(placeholder);
}

/**
 * Finds the top-level field that 'expr' is over. Returns boost::none if the expression does not
 * have a top-level field name, or a non-OK status if there are multiple top-level field names.
 */
StatusWith<boost::optional<std::string_view>> parseTopLevelFieldName(MatchExpression* expr) {
    if (auto pathExpr = dynamic_cast<PathMatchExpression*>(expr)) {
        auto firstDotPos = pathExpr->path().find('.');
        if (firstDotPos == std::string::npos) {
            return {pathExpr->path()};
        }
        return {pathExpr->path().substr(0, firstDotPos)};
    } else if (expr->getCategory() == MatchExpression::MatchCategory::kLogical) {
        boost::optional<std::string_view> placeholder;
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
                              str::stream()
                                  << "Expected a single top-level field name, found '"
                                  << *placeholder << "' and '" << *statusWithId.getValue() << "'");
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

// static
StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> ExpressionWithPlaceholder::make(
    std::unique_ptr<MatchExpression> filter) {
    auto statusWithId = parseTopLevelFieldName(filter.get());
    if (!statusWithId.isOK()) {
        return statusWithId.getStatus();
    }

    boost::optional<std::string> placeholder;
    if (statusWithId.getValue()) {
        placeholder = std::string{*statusWithId.getValue()};
        if (!matchesPlaceholderPattern(*placeholder)) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "The top-level field name must be an alphanumeric "
                                           "string beginning with a lowercase letter, found '"
                                        << *placeholder << "'");
        }
    }

    auto exprWithPlaceholder =
        std::make_unique<ExpressionWithPlaceholder>(std::move(placeholder), std::move(filter));
    return {std::move(exprWithPlaceholder)};
}

void ExpressionWithPlaceholder::resetFilter(MatchExpression* filter) {
    _filter.reset(filter);

    // When the filter is reset we potentially can get a totally different expression where a
    // placeholder field is different from the original one, so we need to recompute it whenever
    // we reset the filter.
    auto newPlaceholder = parseTopLevelFieldName(_filter.get());
    invariant(newPlaceholder.getStatus());

    if (newPlaceholder.getValue()) {
        _placeholder = std::string{*newPlaceholder.getValue()};
        dassert(matchesPlaceholderPattern(*_placeholder));
    } else {
        _placeholder = boost::none;
    }
}

}  // namespace mongo
