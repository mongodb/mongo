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

#pragma once

#include <boost/optional.hpp>
#include <regex>

#include "mongo/base/status_with.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"

namespace mongo {

/**
 * Container for a parsed MatchExpression and top-level field name that it's over.
 * For example, {"i.a": 0, "i.b": 1} is a filter with a single top-level field name "i".
 */
class ExpressionWithPlaceholder {

public:
    static const std::regex placeholderRegex;

    /**
     * Constructs an ExpressionWithPlaceholder from an existing match expression. Returns a non-OK
     * status if the paths inside the match expression do not name a consistent placeholder string.
     */
    static StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> make(
        std::unique_ptr<MatchExpression> filter);

    /**
     * Construct a new ExpressionWithPlaceholder. 'filter' must point to a valid MatchExpression.
     */
    ExpressionWithPlaceholder(boost::optional<std::string> placeholder,
                              std::unique_ptr<MatchExpression> filter)
        : _placeholder(std::move(placeholder)), _filter(std::move(filter)) {
        invariant(static_cast<bool>(_filter));
    }

    /**
     * Returns true if this expression has both a placeholder and filter equivalent to 'other'.
     */
    bool equivalent(const ExpressionWithPlaceholder* other) const;

    /**
     * Uses this filter to match against 'elem' as if it is wrapped in a BSONObj with a single
     * field whose name is given by getPlaceholder(). If the placeholder name does not exist, then
     * the filter expression does not refer to any specific paths.
     */
    bool matchesBSONElement(BSONElement elem, MatchDetails* details = nullptr) const {
        return _filter->matchesBSONElement(elem, details);
    }

    /**
     * If this object has a placeholder, returns a view of the placeholder as a StringData.
     */
    boost::optional<StringData> getPlaceholder() const {
        if (_placeholder) {
            return StringData(*_placeholder);
        }
        return boost::none;
    }

    MatchExpression* getFilter() const {
        return _filter.get();
    }

    std::unique_ptr<ExpressionWithPlaceholder> shallowClone() const {
        return stdx::make_unique<ExpressionWithPlaceholder>(_placeholder, _filter->shallowClone());
    }

    /*
     * Uses MatchExpression::optimize() to replace the Expression part of this
     * ExpressionWithPlaceholder with an optimized expression. If the rewritten expression operates
     * on a different field, we also update the placeholder to match.
     */
    void optimizeFilter();

private:
    // The top-level field that _filter is over.
    boost::optional<std::string> _placeholder;
    std::unique_ptr<MatchExpression> _filter;
};

}  // namespace mongo
