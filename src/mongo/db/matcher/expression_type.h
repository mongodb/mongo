
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

#pragma once

#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/matcher_type_set.h"

namespace mongo {

template <class T>
class TypeMatchExpressionBase : public LeafMatchExpression {
public:
    explicit TypeMatchExpressionBase(MatchType matchType,
                                     StringData path,
                                     ElementPath::LeafArrayBehavior leafArrBehavior,
                                     MatcherTypeSet typeSet)
        : LeafMatchExpression(
              matchType, path, leafArrBehavior, ElementPath::NonLeafArrayBehavior::kTraverse),
          _typeSet(std::move(typeSet)) {}

    virtual ~TypeMatchExpressionBase() = default;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr = stdx::make_unique<T>(path(), _typeSet);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return std::move(expr);
    }

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        return _typeSet.hasType(elem.type());
    }

    void debugString(StringBuilder& debug, int level) const final {
        _debugAddSpace(debug, level);
        debug << path() << " " << name() << ": " << _typeSet.toBSONArray().toString();

        MatchExpression::TagData* td = getTag();
        if (td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    void serialize(BSONObjBuilder* out) const final {
        BSONObjBuilder subBuilder(out->subobjStart(path()));
        BSONArrayBuilder arrBuilder(subBuilder.subarrayStart(name()));
        _typeSet.toBSONArray(&arrBuilder);
        arrBuilder.doneFast();
        subBuilder.doneFast();
    }

    bool equivalent(const MatchExpression* other) const final {
        if (matchType() != other->matchType())
            return false;

        auto realOther = static_cast<const T*>(other);

        if (path() != realOther->path()) {
            return false;
        }

        return _typeSet == realOther->_typeSet;
    }

    /**
     * Returns a representation of the set of matching types.
     */
    const MatcherTypeSet& typeSet() const {
        return _typeSet;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    // The set of matching types.
    MatcherTypeSet _typeSet;
};

class TypeMatchExpression final : public TypeMatchExpressionBase<TypeMatchExpression> {
public:
    static constexpr StringData kName = "$type"_sd;

    TypeMatchExpression(StringData path, MatcherTypeSet typeSet)
        : TypeMatchExpressionBase(MatchExpression::TYPE_OPERATOR,
                                  path,
                                  ElementPath::LeafArrayBehavior::kTraverse,
                                  typeSet) {}

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

    InternalSchemaTypeExpression(StringData path, MatcherTypeSet typeSet)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  typeSet) {}

    StringData name() const final {
        return kName;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }
};

class InternalSchemaBinDataSubTypeExpression final : public LeafMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaBinDataSubType"_sd;

    InternalSchemaBinDataSubTypeExpression(StringData path, BinDataType binDataSubType)
        : LeafMatchExpression(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE,
                              path,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse),
          _binDataSubType(binDataSubType) {}

    virtual ~InternalSchemaBinDataSubTypeExpression() = default;

    StringData name() const {
        return kName;
    }

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        return elem.type() == BSONType::BinData && elem.binDataType() == _binDataSubType;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr =
            stdx::make_unique<InternalSchemaBinDataSubTypeExpression>(path(), _binDataSubType);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return std::move(expr);
    }

    void debugString(StringBuilder& debug, int level) const final {
        _debugAddSpace(debug, level);
        debug << path() << " " << name() << ": " << typeName(_binDataSubType);

        MatchExpression::TagData* td = getTag();
        if (td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    void serialize(BSONObjBuilder* out) const final {
        BSONObjBuilder subBuilder(out->subobjStart(path()));
        subBuilder.append(name(), _binDataSubType);
        subBuilder.doneFast();
    }

    bool equivalent(const MatchExpression* other) const final {
        if (matchType() != other->matchType())
            return false;

        auto realOther = static_cast<const InternalSchemaBinDataSubTypeExpression*>(other);

        if (path() != realOther->path()) {
            return false;
        }

        return _binDataSubType == realOther->_binDataSubType;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    BinDataType _binDataSubType;
};
}  // namespace mongo
