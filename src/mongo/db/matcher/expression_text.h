// expression_text.h

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class NamespaceString;
class OperationContext;

class TextMatchExpression : public TextMatchExpressionBase {
public:
    Status init(OperationContext* opCtx, const NamespaceString& nss, TextParams params);

    const fts::FTSQuery& getFTSQuery() const final {
        return _ftsQuery;
    }

    bool matchesSingleElement(const BSONElement& e, MatchDetails* details = nullptr) const final {
        // Text match expressions force the selection of the text index and always generate EXACT
        // index bounds (which causes the MatchExpression node to be trimmed), so we don't currently
        // implement any explicit text matching logic here. SERVER-17648 tracks the work to
        // implement a real text matcher.
        //
        // TODO: simply returning 'true' here isn't quite correct. First, we should be overriding
        // matches() instead of matchesSingleElement(), because the latter is only ever called if
        // the matched document has an element with path "_fts". Second, there are scenarios where
        // we will use the untrimmed expression tree for matching (for example, when deciding
        // whether or not to retry an operation that throws WriteConflictException); in those cases,
        // we won't be able to correctly determine whether or not the object matches the expression.
        return true;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final;

private:
    fts::FTSQueryImpl _ftsQuery;
};

}  // namespace mongo
