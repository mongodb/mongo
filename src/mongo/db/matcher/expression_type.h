/**
 * Copyright (C) 2017 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/matcher_type_alias.h"

namespace mongo {

template <class T>
class TypeMatchExpressionBase : public LeafMatchExpression {
public:
    explicit TypeMatchExpressionBase(MatchType matchType) : LeafMatchExpression(matchType) {}

    virtual ~TypeMatchExpressionBase() = default;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    Status init(StringData path, MatcherTypeAlias type) {
        _type = std::move(type);
        return setPath(path);
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr = stdx::make_unique<T>();
        invariantOK(expr->init(path(), _type));
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return std::move(expr);
    }

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        return getType().elementMatchesType(elem);
    }

    void debugString(StringBuilder& debug, int level) const final {
        _debugAddSpace(debug, level);
        debug << path() << " " << name() << ": ";
        if (matchesAllNumbers()) {
            debug << MatcherTypeAlias::kMatchesAllNumbersAlias;
        } else {
            debug << _type.bsonType;
        }

        MatchExpression::TagData* td = getTag();
        if (td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    void serialize(BSONObjBuilder* out) const final {
        BSONObjBuilder subBuilder(out->subobjStart(path()));
        if (matchesAllNumbers()) {
            subBuilder.append(name(), MatcherTypeAlias::kMatchesAllNumbersAlias);
        } else {
            subBuilder.append(name(), _type.bsonType);
        }
        subBuilder.doneFast();
    }

    bool equivalent(const MatchExpression* other) const final {
        if (matchType() != other->matchType())
            return false;

        auto realOther = static_cast<const T*>(other);

        if (path() != realOther->path()) {
            return false;
        }

        if (_type.allNumbers) {
            return realOther->_type.allNumbers;
        }

        return _type.bsonType == realOther->_type.bsonType;
    }

    BSONType getBSONType() const {
        return _type.bsonType;
    }

    MatcherTypeAlias getType() const {
        return _type;
    }

    /**
     * Whether or not to match against all number types (NumberDouble, NumberLong, and NumberInt).
     * Defaults to false. If this is true, _type is EOO.
     */
    bool matchesAllNumbers() const {
        return _type.allNumbers;
    }

private:
    MatcherTypeAlias _type;
};

class TypeMatchExpression final : public TypeMatchExpressionBase<TypeMatchExpression> {
public:
    static constexpr StringData kName = "$type"_sd;

    TypeMatchExpression() : TypeMatchExpressionBase(MatchExpression::TYPE_OPERATOR) {}

    StringData name() const final {
        return kName;
    }
};

/**
 * Implements matching semantics for the JSON Schema type keyword. Although the MongoDB query
 * language has a $type operator, its meaning for arrays differs from JSON Schema. Therefore, we
 * implement a separate type node for schema matching.
 */
class InternalSchemaTypeExpression final
    : public TypeMatchExpressionBase<InternalSchemaTypeExpression> {
public:
    static constexpr StringData kName = "$_internalSchemaType"_sd;

    InternalSchemaTypeExpression()
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_TYPE) {}

    StringData name() const final {
        return kName;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    bool shouldExpandLeafArray() const final {
        return false;
    }
};

}  // namespace mongo
