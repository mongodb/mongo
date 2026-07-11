// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/util.h"

namespace mongo {
namespace sbe {
namespace value {

size_t getArraySize(TypeTags tag, Value value) {
    size_t result = 0;
    switch (tag) {
        case TypeTags::Array: {
            result = value::getArrayView(value)->size();
            break;
        }
        case TypeTags::ArraySet: {
            result = value::getArraySetView(value)->size();
            break;
        }
        case TypeTags::ArrayMultiSet: {
            result = value::getArrayMultiSetView(value)->size();
            break;
        }
        case TypeTags::bsonArray: {
            arrayForEach(
                tag, value, [&](value::TypeTags t_unused, value::Value v_unused) { result++; });
            break;
        }
        default:
            tasserted(10386600, "Cannot retrieve a length from a non-array type");
    }
    return result;
}

}  // namespace value
}  // namespace sbe
}  // namespace mongo
