/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/column/interleaved_schema.h"

namespace mongo {

InterleavedSchema::InterleavedSchema(const BSONObj& referenceObj, BSONType rootType, bool arrays)
    : _arrays(arrays) {
    uint16_t scalarIdx = 0;
    _discover(referenceObj, ""_sd, rootType, referenceObj.isEmpty(), scalarIdx);
    _scalarCount = scalarIdx;
}

void InterleavedSchema::_discover(
    const BSONObj& obj, StringData fieldName, BSONType type, bool allowEmpty, uint16_t& scalarIdx) {
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
