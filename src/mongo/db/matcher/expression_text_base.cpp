/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/matcher/expression_text_base.h"

#include "mongo/db/fts/fts_query.h"

namespace mongo {

const bool TextMatchExpressionBase::kCaseSensitiveDefault = false;
const bool TextMatchExpressionBase::kDiacriticSensitiveDefault = false;

TextMatchExpressionBase::TextMatchExpressionBase() : LeafMatchExpression(TEXT) {}

void TextMatchExpressionBase::debugString(StringBuilder& debug, int level) const {
    const fts::FTSQuery& ftsQuery = getFTSQuery();
    _debugAddSpace(debug, level);
    debug << "TEXT : query=" << ftsQuery.getQuery() << ", language=" << ftsQuery.getLanguage()
          << ", caseSensitive=" << ftsQuery.getCaseSensitive()
          << ", diacriticSensitive=" << ftsQuery.getDiacriticSensitive() << ", tag=";
    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        td->debugString(&debug);
    } else {
        debug << "NULL";
    }
    debug << "\n";
}

void TextMatchExpressionBase::serialize(BSONObjBuilder* out) const {
    const fts::FTSQuery& ftsQuery = getFTSQuery();
    out->append("$text",
                BSON("$search" << ftsQuery.getQuery() << "$language" << ftsQuery.getLanguage()
                               << "$caseSensitive"
                               << ftsQuery.getCaseSensitive()
                               << "$diacriticSensitive"
                               << ftsQuery.getDiacriticSensitive()));
}

bool TextMatchExpressionBase::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const TextMatchExpressionBase* realOther = static_cast<const TextMatchExpressionBase*>(other);

    return getFTSQuery().equivalent(realOther->getFTSQuery());
}

}  // namespace mongo
