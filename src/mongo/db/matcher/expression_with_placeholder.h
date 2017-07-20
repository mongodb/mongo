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

#include <regex>

#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

/**
 * Container for a parsed MatchExpression and top-level field name that it's over.
 * For example, {"i.a": 0, "i.b": 1} is a filter with a single top-level field name "i".
 */
class ExpressionWithPlaceholder {

public:
    static const std::regex placeholderRegex;

    /**
     * Parses 'rawFilter' to an ExpressionWithPlaceholder. This succeeds if 'rawFilter' is a
     * filter over a single top-level field, which begins with a lowercase letter and contains
     * no special characters. Otherwise, a non-OK status is returned. Callers must maintain
     * ownership of 'rawFilter'.
     */
    static StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> parse(
        BSONObj rawFilter, const CollatorInterface* collator);

    ExpressionWithPlaceholder(std::string placeholder, std::unique_ptr<MatchExpression> filter)
        : _placeholder(std::move(placeholder)), _filter(std::move(filter)) {}

    StringData getPlaceholder() const {
        return _placeholder;
    }

    MatchExpression* getFilter() const {
        return _filter.get();
    }

private:
    // The top-level field that _filter is over.
    const std::string _placeholder;
    const std::unique_ptr<MatchExpression> _filter;
};

}  // namespace mongo
