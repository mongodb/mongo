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

#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/matcher_type_set.h"

namespace mongo {

/**
 * The structure represents how data is laid out in an encrypted payload.
 */
struct FleBlobHeader {
    int8_t fleBlobSubtype;
    int8_t keyUUID[16];
    int8_t originalBsonType;
};

template <class T>
class TypeMatchExpressionBase : public LeafMatchExpression {
public:
    explicit TypeMatchExpressionBase(MatchType matchType,
                                     StringData path,
                                     ElementPath::LeafArrayBehavior leafArrBehavior,
                                     MatcherTypeSet typeSet,
                                     clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(matchType,
                              path,
                              leafArrBehavior,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)),
          _typeSet(std::move(typeSet)) {}

    virtual ~TypeMatchExpressionBase() = default;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual StringData name() const = 0;

    bool matchesSingleElement(const BSONElement& elem, MatchDetails* details = nullptr) const {
        return _typeSet.hasType(elem.type());
    }

    void debugString(StringBuilder& debug, int indentationLevel) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << path() << " " << name() << ": " << _typeSet.toBSONArray().toString();

        MatchExpression::TagData* td = getTag();
        if (td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    BSONObj getSerializedRightHandSide() const final {
        BSONObjBuilder subBuilder;
        BSONArrayBuilder arrBuilder(subBuilder.subarrayStart(name()));
        _typeSet.toBSONArray(&arrBuilder);
        arrBuilder.doneFast();
        return subBuilder.obj();
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

    TypeMatchExpression(StringData path,
                        MatcherTypeSet typeSet,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::TYPE_OPERATOR,
                                  path,
                                  ElementPath::LeafArrayBehavior::kTraverse,
                                  typeSet,
                                  std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr = std::make_unique<TypeMatchExpression>(path(), typeSet(), _errorAnnotation);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        if (getInputParamId()) {
            expr->setInputParamId(*getInputParamId());
        }
        return expr;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void setInputParamId(InputParamId paramId) {
        _inputParamId = paramId;
    }

    boost::optional<InputParamId> getInputParamId() const {
        return _inputParamId;
    }

private:
    boost::optional<InputParamId> _inputParamId;
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

    InternalSchemaTypeExpression(StringData path,
                                 MatcherTypeSet typeSet,
                                 clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  typeSet,
                                  std::move(annotation)) {}

    StringData name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr =
            std::make_unique<InternalSchemaTypeExpression>(path(), typeSet(), _errorAnnotation);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return expr;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

class InternalSchemaBinDataSubTypeExpression final : public LeafMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaBinDataSubType"_sd;

    InternalSchemaBinDataSubTypeExpression(StringData path,
                                           BinDataType binDataSubType,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE,
                              path,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)),
          _binDataSubType(binDataSubType) {}

    StringData name() const {
        return kName;
    }

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        return elem.type() == BSONType::BinData && elem.binDataType() == _binDataSubType;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr = std::make_unique<InternalSchemaBinDataSubTypeExpression>(
            path(), _binDataSubType, _errorAnnotation);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return expr;
    }

    void debugString(StringBuilder& debug, int indentationLevel) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << path() << " " << name() << ": " << typeName(_binDataSubType);

        MatchExpression::TagData* td = getTag();
        if (td) {
            debug << " ";
            td->debugString(&debug);
        }
        debug << "\n";
    }

    BSONObj getSerializedRightHandSide() const final {
        BSONObjBuilder bob;
        bob.append(name(), _binDataSubType);
        return bob.obj();
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

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    BinDataType _binDataSubType;
};

/**
 * Implements matching semantics for the JSON Schema keyword encrypt.bsonType. A document
 * matches successfully if a field is encrypted and the encrypted payload indicates the
 * original BSON element belongs to the specified type set.
 */
class InternalSchemaBinDataEncryptedTypeExpression final
    : public TypeMatchExpressionBase<InternalSchemaBinDataEncryptedTypeExpression> {
public:
    static constexpr StringData kName = "$_internalSchemaBinDataEncryptedType"_sd;

    InternalSchemaBinDataEncryptedTypeExpression(StringData path,
                                                 MatcherTypeSet typeSet,
                                                 clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  typeSet,
                                  std::move(annotation)) {}

    StringData name() const {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr = std::make_unique<InternalSchemaBinDataEncryptedTypeExpression>(
            path(), typeSet(), _errorAnnotation);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return expr;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        if (elem.type() != BSONType::BinData)
            return false;
        if (elem.binDataType() != BinDataType::Encrypt)
            return false;

        int binDataLen;
        auto binData = elem.binData(binDataLen);
        if (static_cast<size_t>(binDataLen) < sizeof(FleBlobHeader))
            return false;

        auto fleBlobSubType = EncryptedBinDataType_parse(IDLParserContext("subtype"), binData[0]);
        switch (fleBlobSubType) {
            case EncryptedBinDataType::kDeterministic:
            case EncryptedBinDataType::kRandom: {
                // Verify the type of the encrypted data.
                auto fleBlob = reinterpret_cast<const FleBlobHeader*>(binData);
                return typeSet().hasType(static_cast<BSONType>(fleBlob->originalBsonType));
            }
            default:
                return false;
        }
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

/**
 * Implements matching semantics for a FLE2 encrypted field. A document
 * matches successfully if a field is encrypted and the encrypted payload indicates the
 * original BSON element belongs to the specified type set.
 */
class InternalSchemaBinDataFLE2EncryptedTypeExpression final
    : public TypeMatchExpressionBase<InternalSchemaBinDataFLE2EncryptedTypeExpression> {
public:
    static constexpr StringData kName = "$_internalSchemaBinDataFLE2EncryptedType"_sd;

    InternalSchemaBinDataFLE2EncryptedTypeExpression(
        StringData path, MatcherTypeSet typeSet, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  std::move(typeSet),
                                  std::move(annotation)) {}

    StringData name() const {
        return kName;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final {
        auto expr = std::make_unique<InternalSchemaBinDataFLE2EncryptedTypeExpression>(
            path(), typeSet(), _errorAnnotation);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return expr;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        if (elem.type() != BSONType::BinData)
            return false;
        if (elem.binDataType() != BinDataType::Encrypt)
            return false;

        int binDataLen;
        auto binData = elem.binData(binDataLen);
        if (static_cast<size_t>(binDataLen) < sizeof(FleBlobHeader))
            return false;

        EncryptedBinDataType subTypeByte = static_cast<EncryptedBinDataType>(binData[0]);
        switch (subTypeByte) {
            case EncryptedBinDataType::kFLE2EqualityIndexedValue:
            case EncryptedBinDataType::kFLE2UnindexedEncryptedValue: {
                // Verify the type of the encrypted data.
                if (typeSet().isEmpty()) {
                    return true;
                } else {
                    auto fleBlob = reinterpret_cast<const FleBlobHeader*>(binData);
                    return typeSet().hasType(static_cast<BSONType>(fleBlob->originalBsonType));
                }
            }
            default:
                return false;
        }
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

}  // namespace mongo
