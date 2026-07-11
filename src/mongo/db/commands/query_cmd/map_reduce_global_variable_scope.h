// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Parsed MapReduce Global variable scope.
 */
class MapReduceGlobalVariableScope {
public:
    static MapReduceGlobalVariableScope parseFromBSON(const BSONElement& element) {
        if (element.type() == BSONType::null) {
            return MapReduceGlobalVariableScope();
        }
        uassert(
            ErrorCodes::BadValue, "'scope' must be an object", element.type() == BSONType::object);
        return MapReduceGlobalVariableScope(element.embeddedObject());
    }

    MapReduceGlobalVariableScope() = default;
    MapReduceGlobalVariableScope(const BSONObj& obj) : obj(boost::make_optional(obj.getOwned())) {}

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
        if (obj == boost::none) {
            builder->append(fieldName, "null");
        }
        builder->append(fieldName, obj.get());
    }

    boost::optional<BSONObj> getObj() const {
        return obj;
    }

private:
    // Initializers for global variables. These will be directly executed as Javascript. This is
    // left as a BSONObj for that API.
    boost::optional<BSONObj> obj;
};

}  // namespace mongo
