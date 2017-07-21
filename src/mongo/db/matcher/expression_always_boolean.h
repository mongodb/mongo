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

#include "mongo/db/matcher/expression.h"

namespace mongo {

class AlwaysBooleanMatchExpression : public MatchExpression {
public:
    AlwaysBooleanMatchExpression(MatchType type, bool value)
        : MatchExpression(type), _value(value) {}

    virtual ~AlwaysBooleanMatchExpression() = default;

    /**
     * The name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final {
        return _value;
    }

    bool matchesSingleElement(const BSONElement& e) const final {
        return _value;
    }

    void debugString(StringBuilder& debug, int level = 0) const final {
        _debugAddSpace(debug, level);
        debug << name() << ": 1\n";
    }

    void serialize(BSONObjBuilder* out) const final {
        out->append(name(), 1);
    }

    bool equivalent(const MatchExpression* other) const final {
        return other->matchType() == matchType();
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

private:
    bool _value;
};

class AlwaysFalseMatchExpression final : public AlwaysBooleanMatchExpression {
public:
    AlwaysFalseMatchExpression() : AlwaysBooleanMatchExpression(MatchType::ALWAYS_FALSE, false) {}

    StringData name() const final {
        return "$alwaysFalse"_sd;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        return stdx::make_unique<AlwaysFalseMatchExpression>();
    }
};

class AlwaysTrueMatchExpression final : public AlwaysBooleanMatchExpression {
public:
    AlwaysTrueMatchExpression() : AlwaysBooleanMatchExpression(MatchType::ALWAYS_TRUE, true) {}

    StringData name() const final {
        return "$alwaysTrue"_sd;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        return stdx::make_unique<AlwaysTrueMatchExpression>();
    }
};

}  // namespace mongo
