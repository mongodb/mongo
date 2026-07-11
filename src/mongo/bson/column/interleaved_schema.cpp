// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/column/interleaved_schema.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

InterleavedSchema::InterleavedSchema(const BSONObj& referenceObj, BSONType rootType, bool arrays)
    : _arrays(arrays) {
    index_t scalarIdx = 0;
    _discover(referenceObj, ""sv, rootType, referenceObj.isEmpty(), scalarIdx);
    _scalarCount = scalarIdx;
}

void InterleavedSchema::_discover(const BSONObj& obj,
                                  std::string_view fieldName,
                                  BSONType type,
                                  bool allowEmpty,
                                  index_t& scalarIdx) {
    _entries.push_back({Op::kEnterSubObj, fieldName, 0, type, allowEmpty});

    for (auto&& elem : obj) {
        bool isSubObj = _arrays
            ? (elem.type() == BSONType::object || elem.type() == BSONType::array)
            : (elem.type() == BSONType::object);
        if (isSubObj) {
            _discover(elem.Obj(),
                      elem.fieldNameStringData(),
                      elem.type(),
                      elem.Obj().isEmpty(),
                      scalarIdx);
        } else {
            _entries.push_back(
                {Op::kScalar, elem.fieldNameStringData(), scalarIdx++, BSONType::eoo, false});
        }
    }

    _entries.push_back({Op::kExitSubObj, fieldName, 0, type, allowEmpty});
}

}  // namespace mongo
