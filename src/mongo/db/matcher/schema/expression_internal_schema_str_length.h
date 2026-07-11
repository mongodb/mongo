// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

class InternalSchemaStrLengthMatchExpression : public LeafMatchExpression {
public:
    using Validator = std::function<bool(int)>;

    InternalSchemaStrLengthMatchExpression(MatchType type,
                                           boost::optional<std::string_view> path,
                                           long long strLen,
                                           std::string_view name,
                                           clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ~InternalSchemaStrLengthMatchExpression() override {}

    virtual Validator getComparator() const = 0;

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    void appendSerializedRightHandSide(BSONObjBuilder* bob,
                                       const query_shape::SerializationOptions& opts = {},
                                       bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    std::string_view getName() const {
        return _name;
    }

    long long strLen() const {
        return _strLen;
    }

private:
    std::string_view _name;
    long long _strLen = 0;
};

}  // namespace mongo
