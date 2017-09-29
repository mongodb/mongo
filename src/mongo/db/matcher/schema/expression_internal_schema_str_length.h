/**
 * Copyright 2017 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class InternalSchemaStrLengthMatchExpression : public LeafMatchExpression {
public:
    using Validator = stdx::function<bool(int)>;

    InternalSchemaStrLengthMatchExpression(MatchType type, StringData name)
        : LeafMatchExpression(type), _name(name) {}

    virtual ~InternalSchemaStrLengthMatchExpression() {}

    Status init(StringData path, long long strLen) {
        _strLen = strLen;
        return setPath(path);
    }

    virtual Validator getComparator() const = 0;

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        if (elem.type() != BSONType::String) {
            return false;
        }

        auto len = str::lengthInUTF8CodePoints(elem.valueStringData());
        return getComparator()(len);
    };

    void debugString(StringBuilder& debug, int level) const final;

    void serialize(BSONObjBuilder* out) const final;

    bool equivalent(const MatchExpression* other) const final;

protected:
    long long strLen() const {
        return _strLen;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    StringData _name;
    long long _strLen = 0;
};

}  // namespace mongo
