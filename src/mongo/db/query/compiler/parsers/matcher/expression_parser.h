// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <functional>
#include <limits>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class OperationContext;

enum class PathAcceptingKeyword {
    ALL,
    BITS_ALL_CLEAR,
    BITS_ALL_SET,
    BITS_ANY_CLEAR,
    BITS_ANY_SET,
    ELEM_MATCH,
    EQUALITY,
    EXISTS,
    GEO_INTERSECTS,
    GEO_NEAR,
    GREATER_THAN,
    GREATER_THAN_OR_EQUAL,
    INTERNAL_EXPR_EQ,
    INTERNAL_EXPR_GT,
    INTERNAL_EXPR_GTE,
    INTERNAL_EXPR_LT,
    INTERNAL_EXPR_LTE,
    INTERNAL_EQ_HASHED_KEY,
    INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX,
    INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE,
    INTERNAL_SCHEMA_BIN_DATA_SUBTYPE,
    INTERNAL_SCHEMA_EQ,
    INTERNAL_SCHEMA_FMOD,
    INTERNAL_SCHEMA_MATCH_ARRAY_INDEX,
    INTERNAL_SCHEMA_MAX_ITEMS,
    INTERNAL_SCHEMA_MAX_LENGTH,
    INTERNAL_SCHEMA_MIN_ITEMS,
    INTERNAL_SCHEMA_MIN_LENGTH,
    INTERNAL_SCHEMA_OBJECT_MATCH,
    INTERNAL_SCHEMA_TYPE,
    INTERNAL_SCHEMA_UNIQUE_ITEMS,
    IN_EXPR,
    LESS_THAN,
    LESS_THAN_OR_EQUAL,
    MOD,
    NOT_EQUAL,
    NOT_IN,
    OPTIONS,
    REGEX,
    SIZE,
    TYPE,
    WITHIN,
};

class [[MONGO_MOD_PUBLIC]] MatchExpressionParser {
public:
    /**
     * Features allowed in match expression parsing.
     */
    enum AllowedFeatures {
        kText = 1,
        kGeoNear = 1 << 1,
        kJavascript = 1 << 2,
        kExpr = 1 << 3,
        kJSONSchema = 1 << 4,
        kEncryptKeywords = 1 << 5,
    };
    using AllowedFeatureSet = unsigned long long;
    static constexpr AllowedFeatureSet kBanAllSpecialFeatures = 0;
    static constexpr AllowedFeatureSet kAllowAllSpecialFeatures =
        std::numeric_limits<unsigned long long>::max();
    static constexpr AllowedFeatureSet kDefaultSpecialFeatures =
        AllowedFeatures::kExpr | AllowedFeatures::kJSONSchema | AllowedFeatures::kEncryptKeywords;

    /**
     * Parses PathAcceptingKeyword from 'typeElem'. Returns 'defaultKeyword' if 'typeElem'
     * doesn't represent a known type.
     */
    static boost::optional<PathAcceptingKeyword> parsePathAcceptingKeyword(
        BSONElement typeElem, boost::optional<PathAcceptingKeyword> defaultKeyword = boost::none);

    /**
     * Caller has to maintain ownership of 'obj'.
     * The tree has views (BSONElement) into 'obj'.
     */
    static StatusWithMatchExpression parse(
        const BSONObj& obj,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        AllowedFeatureSet allowedFeatures = kDefaultSpecialFeatures);

    /**
     * Parse the given MatchExpression and normalize the resulting tree by optimizing and then
     * sorting it. Throws if the given BSONObj fails to parse.
     */
    static std::unique_ptr<MatchExpression> parseAndNormalize(
        const BSONObj& obj,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        AllowedFeatureSet allowedFeatures = kDefaultSpecialFeatures);
};
}  // namespace mongo
