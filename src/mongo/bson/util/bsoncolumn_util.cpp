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

#include "mongo/bson/util/bsoncolumn_util.h"

namespace mongo::bsoncolumn {
namespace {
/**
 * Helper class to perform recursion over a BSONObj. Two functions are provided:
 *
 * EnterSubObjFunc is called before recursing deeper, it may output an RAII object to perform logic
 * when entering/exiting a subobject.
 *
 * ElementFunc is called for every non-object element encountered during the recursion.
 */
template <typename EnterSubObjFunc, typename ElementFunc>
class BSONObjTraversalUtil {
public:
    BSONObjTraversalUtil(bool recurseIntoArrays,
                         BSONType rootType,
                         EnterSubObjFunc enterFunc,
                         ElementFunc elemFunc)
        : _enterFunc(std::move(enterFunc)),
          _elemFunc(std::move(elemFunc)),
          _recurseIntoArrays(recurseIntoArrays),
          _rootType(rootType) {}

    bool traverse(const BSONObj& obj) {
        if (_recurseIntoArrays) {
            return _traverseIntoArrays(""_sd, obj, _rootType);
        } else {
            return _traverseNoArrays(""_sd, obj, _rootType);
        }
    }

private:
    bool _traverseNoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == Object
                ? _traverseNoArrays(elem.fieldNameStringData(), elem.Obj(), Object)
                : _elemFunc(elem);
        });
    }

    bool _traverseIntoArrays(StringData fieldName, const BSONObj& obj, BSONType type) {
        [[maybe_unused]] auto raii = _enterFunc(fieldName, obj, type);

        return std::all_of(obj.begin(), obj.end(), [this, &fieldName](auto&& elem) {
            return elem.type() == Object || elem.type() == Array
                ? _traverseIntoArrays(elem.fieldNameStringData(), elem.Obj(), elem.type())
                : _elemFunc(elem);
        });
    }

    EnterSubObjFunc _enterFunc;
    ElementFunc _elemFunc;
    bool _recurseIntoArrays;
    BSONType _rootType;
};
}  // namespace

uint32_t numInterleavedStreams(const BSONObj& refObj, uint8_t control) {
    BSONType rootType =
        static_cast<char>(control) == bsoncolumn::kInterleavedStartArrayRootControlByte ? Array
                                                                                        : Object;

    bool traverseIntoArrays =
        static_cast<char>(control) == bsoncolumn::kInterleavedStartControlByte ||
        static_cast<char>(control) == bsoncolumn::kInterleavedStartArrayRootControlByte;

    uint32_t num = 0;
    BSONObjTraversalUtil t(
        traverseIntoArrays,
        rootType,
        [](StringData fieldName, const BSONObj& obj, BSONType type) { return true; },
        [&num](const BSONElement& elem) {
            ++num;
            return true;
        });
    t.traverse(refObj);
    uassert(11761702, "Invalid BSONColumn interleaved reference object for control byte", num != 0);

    return num;
}

}  // namespace mongo::bsoncolumn
