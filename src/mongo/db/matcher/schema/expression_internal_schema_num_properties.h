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

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>
#include <string>
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
                   const SerializationOptions& opts = {},
                   bool includePath = true) const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    long long numProperties() const {
        return _numProperties;
    }

    StringData getName() const {
        return _name;
    }

private:
    long long _numProperties;
    std::string _name;
};
}  // namespace mongo
