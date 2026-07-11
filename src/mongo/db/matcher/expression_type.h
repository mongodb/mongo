// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/path.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <sys/types.h>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * The structure represents how data is laid out in an encrypted payload.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] FleBlobHeader {
    int8_t fleBlobSubtype;
    int8_t keyUUID[16];
    int8_t originalBsonType;
};

template <class T>
class TypeMatchExpressionBase : public LeafMatchExpression {
public:
    explicit TypeMatchExpressionBase(MatchType matchType,
                                     boost::optional<std::string_view> path,
                                     ElementPath::LeafArrayBehavior leafArrBehavior,
                                     MatcherTypeSet typeSet,
                                     clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(matchType,
                              path,
                              leafArrBehavior,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)),
          _typeSet(std::move(typeSet)) {}

    ~TypeMatchExpressionBase() override = default;

    /**
     * Returns the name of this MatchExpression.
     */
    virtual std::string_view name() const = 0;

    void debugString(StringBuilder& debug, int indentationLevel) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << path() << " " << name() << ": " << _typeSet.toBSONArray().toString();
        _debugStringAttachTagInfo(&debug);
    }

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final {
        bob->appendArray(name(), _typeSet.toBSONArray());
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
    // The set of matching types.
    MatcherTypeSet _typeSet;
};

class TypeMatchExpression final : public TypeMatchExpressionBase<TypeMatchExpression> {
public:
    static constexpr std::string_view kName = "$type"sv;

    TypeMatchExpression(boost::optional<std::string_view> path,
                        MatcherTypeSet typeSet,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::TYPE_OPERATOR,
                                  path,
                                  ElementPath::LeafArrayBehavior::kTraverse,
                                  typeSet,
                                  std::move(annotation)) {}

    std::string_view name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
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
    static constexpr std::string_view kName = "$_internalSchemaType"sv;

    InternalSchemaTypeExpression(boost::optional<std::string_view> path,
                                 MatcherTypeSet typeSet,
                                 clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  typeSet,
                                  std::move(annotation)) {}

    std::string_view name() const final {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
        auto expr =
            std::make_unique<InternalSchemaTypeExpression>(path(), typeSet(), _errorAnnotation);
        if (getTag()) {
            expr->setTag(getTag()->clone());
        }
        return expr;
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
    static constexpr std::string_view kName = "$_internalSchemaBinDataSubType"sv;

    InternalSchemaBinDataSubTypeExpression(boost::optional<std::string_view> path,
                                           BinDataType binDataSubType,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : LeafMatchExpression(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE,
                              path,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse,
                              std::move(annotation)),
          _binDataSubType(binDataSubType) {}

    std::string_view name() const {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
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
        _debugStringAttachTagInfo(&debug);
    }

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final {
        if (opts.isKeepingLiteralsUnchanged()) {
            bob->append(name(), _binDataSubType);
        } else {
            // There is some fancy serialization logic to get the above BSONObjBuilder append to
            // work. We just want to make sure we're doing the same thing here.
            static_assert(BSONObjAppendFormat<decltype(_binDataSubType)>::value ==
                              BSONType::numberInt,
                          "Expecting that the BinData sub type should be specified and serialized "
                          "as an int.");
            opts.appendLiteral(bob, name(), static_cast<int>(_binDataSubType));
        }
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

    BinDataType getBinDataSubType() const {
        return _binDataSubType;
    }

private:
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
    static constexpr std::string_view kName = "$_internalSchemaBinDataEncryptedType"sv;

    InternalSchemaBinDataEncryptedTypeExpression(boost::optional<std::string_view> path,
                                                 MatcherTypeSet typeSet,
                                                 clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  typeSet,
                                  std::move(annotation)) {}

    std::string_view name() const override {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
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
    static constexpr std::string_view kName = "$_internalSchemaBinDataFLE2EncryptedType"sv;

    InternalSchemaBinDataFLE2EncryptedTypeExpression(
        boost::optional<std::string_view> path,
        MatcherTypeSet typeSet,
        clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : TypeMatchExpressionBase(MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE,
                                  path,
                                  ElementPath::LeafArrayBehavior::kNoTraversal,
                                  std::move(typeSet),
                                  std::move(annotation)) {}

    std::string_view name() const override {
        return kName;
    }

    std::unique_ptr<MatchExpression> clone() const final {
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

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }
};

}  // namespace mongo
