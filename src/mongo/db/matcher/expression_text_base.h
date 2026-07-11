// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {

namespace fts {
class FTSQuery;
}  // namespace fts

/**
 * Common base class for $text match expression implementations.
 */
class TextMatchExpressionBase : public LeafMatchExpression {
public:
    struct TextParams {
        std::string query;
        std::string language;
        bool caseSensitive;
        bool diacriticSensitive;
    };

    static const bool kCaseSensitiveDefault;
    static const bool kDiacriticSensitiveDefault;

    explicit TextMatchExpressionBase(std::string_view path);
    ~TextMatchExpressionBase() override {}

    /**
     * Returns a reference to the parsed text query that this TextMatchExpressionBase owns.
     */
    virtual const fts::FTSQuery& getFTSQuery() const = 0;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final {
        MONGO_UNREACHABLE;
    }

    //
    // Methods inherited from MatchExpression.
    //

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;
};

}  // namespace mongo
