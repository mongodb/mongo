/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_text_noop.h"

#include "mongo/base/string_data.h"
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
