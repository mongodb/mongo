// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Container for a parsed MatchExpression and top-level field name that it's over.
 * For example, {"i.a": 0, "i.b": 1} is a filter with a single top-level field name "i".
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ExpressionWithPlaceholder {

public:
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
        tassert(11052418, "filter must not be null", _filter);
    }

    /**
     * Returns true if this expression has both a placeholder and filter equivalent to 'other'.
     */
    bool equivalent(const ExpressionWithPlaceholder* other) const;

    /**
     * If this object has a placeholder, returns a view of the placeholder as a std::string_view.
     */
    boost::optional<std::string_view> getPlaceholder() const {
        if (_placeholder) {
            return std::string_view(*_placeholder);
        }
        return boost::none;
    }

    MatchExpression* getFilter() const {
        return _filter.get();
    }

    MatchExpression* releaseFilter() {
        return _filter.release();
    }

    void resetFilter(MatchExpression* other);

    std::unique_ptr<ExpressionWithPlaceholder> clone() const {
        return std::make_unique<ExpressionWithPlaceholder>(_placeholder, _filter->clone());
    }

private:
    // The top-level field that _filter is over.
    boost::optional<std::string> _placeholder;
    std::unique_ptr<MatchExpression> _filter;
};

}  // namespace mongo
