// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

/**
 * MatchExpression for internal JSON Schema keywords that validate the number of properties in an
 * object.
 */
class InternalSchemaNumPropertiesMatchExpression : public MatchExpression {
public:
    InternalSchemaNumPropertiesMatchExpression(MatchType type,
                                               long long numProperties,
                                               std::string name,
                                               clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(type, std::move(annotation)),
          _numProperties(numProperties),
          _name(name) {}

    ~InternalSchemaNumPropertiesMatchExpression() override {}

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400216);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    void serialize(BSONObjBuilder* out,
                   const query_shape::SerializationOptions& opts = {},
                   bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    long long numProperties() const {
        return _numProperties;
    }

    std::string_view getName() const {
        return _name;
    }

private:
    long long _numProperties;
    std::string _name;
};
}  // namespace mongo
