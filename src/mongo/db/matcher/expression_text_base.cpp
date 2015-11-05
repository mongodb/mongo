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

namespace mongo {

const bool TextMatchExpressionBase::kCaseSensitiveDefault = false;
const bool TextMatchExpressionBase::kDiacriticSensitiveDefault = false;

TextMatchExpressionBase::TextMatchExpressionBase(TextParams params)
    : LeafMatchExpression(TEXT),
      _query(std::move(params.query)),
      _language(std::move(params.language)),
      _caseSensitive(params.caseSensitive),
      _diacriticSensitive(params.diacriticSensitive) {}

void TextMatchExpressionBase::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << "TEXT : query=" << _query << ", language=" << _language
          << ", caseSensitive=" << _caseSensitive << ", diacriticSensitive=" << _diacriticSensitive
          << ", tag=";
    MatchExpression::TagData* td = getTag();
    if (NULL != td) {
        td->debugString(&debug);
    } else {
        debug << "NULL";
    }
    debug << "\n";
}

void TextMatchExpressionBase::toBSON(BSONObjBuilder* out) const {
    out->append("$text",
                BSON("$search" << _query << "$language" << _language << "$caseSensitive"
                               << _caseSensitive << "$diacriticSensitive" << _diacriticSensitive));
}

bool TextMatchExpressionBase::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const TextMatchExpressionBase* realOther = static_cast<const TextMatchExpressionBase*>(other);

    if (realOther->getQuery() != _query) {
        return false;
    }
    if (realOther->getLanguage() != _language) {
        return false;
    }
    if (realOther->getCaseSensitive() != _caseSensitive) {
        return false;
    }
    if (realOther->getDiacriticSensitive() != _diacriticSensitive) {
        return false;
    }
    return true;
}

}  // namespace mongo
