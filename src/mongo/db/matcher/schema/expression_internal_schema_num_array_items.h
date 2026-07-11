// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * MatchExpression for internal JSON Schema keywords that validate the number of items in an array.
 */
class InternalSchemaNumArrayItemsMatchExpression : public ArrayMatchingMatchExpression {
public:
    InternalSchemaNumArrayItemsMatchExpression(MatchType type,
                                               boost::optional<std::string_view> path,
                                               long long numItems,
                                               std::string_view name,
                                               clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ~InternalSchemaNumArrayItemsMatchExpression() override {}

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400215);
    }

    void resetChild(size_t i, MatchExpression* other) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    std::string_view getName() const {
        return _name;
    }

    long long numItems() const {
        return _numItems;
    }

private:
    std::string_view _name;
    long long _numItems = 0;
};
}  // namespace mongo
