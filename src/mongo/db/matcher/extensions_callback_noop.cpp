// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/extensions_callback_noop.h"

#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_where_noop.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

std::unique_ptr<MatchExpression> ExtensionsCallbackNoop::createText(
    TextMatchExpressionBase::TextParams text) const {
    return std::make_unique<TextNoOpMatchExpression>(std::move(text));
}

std::unique_ptr<MatchExpression> ExtensionsCallbackNoop::createWhere(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    WhereMatchExpressionBase::WhereParams where) const {
    return std::make_unique<WhereNoOpMatchExpression>(std::move(where));
}

}  // namespace mongo
