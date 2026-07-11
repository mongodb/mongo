// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_text_noop.h"

#include "mongo/db/fts/fts_util.h"

#include <memory>
#include <string>
#include <utility>


namespace mongo {

TextNoOpMatchExpression::TextNoOpMatchExpression(TextParams params)
    : TextMatchExpressionBase("_fts") {
    _ftsQuery.setQuery(std::move(params.query));
    _ftsQuery.setLanguage(std::move(params.language));
    _ftsQuery.setCaseSensitive(params.caseSensitive);
    _ftsQuery.setDiacriticSensitive(params.diacriticSensitive);
    tassert(_ftsQuery.parse(fts::TEXT_INDEX_VERSION_INVALID));
}

std::unique_ptr<MatchExpression> TextNoOpMatchExpression::clone() const {
    TextParams params;
    params.query = _ftsQuery.getQuery();
    params.language = _ftsQuery.getLanguage();
    params.caseSensitive = _ftsQuery.getCaseSensitive();
    params.diacriticSensitive = _ftsQuery.getDiacriticSensitive();

    auto expr = std::make_unique<TextNoOpMatchExpression>(std::move(params));
    if (getTag()) {
        expr->setTag(getTag()->clone());
    }
    return expr;
}

}  // namespace mongo
