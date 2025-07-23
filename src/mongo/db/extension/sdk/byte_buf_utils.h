/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/util/assert_util.h"

#include <string_view>


namespace mongo::extension::sdk {
inline std::string_view byteViewAsStringView(const MongoExtensionByteView& view) {
    return std::string_view(reinterpret_cast<const char*>(view.data), view.len);
}

inline BSONObj bsonObjFromByteView(const MongoExtensionByteView& view) {
    auto isValidObj = [](const MongoExtensionByteView& view) -> bool {
        if (view.len < BSONObj::kMinBSONLength) {
            return false;
        }

        // Decode the value in little-endian order.
        int32_t docLength = static_cast<int32_t>(view.data[0]) |
            (static_cast<int32_t>(view.data[1]) << 8) | (static_cast<int32_t>(view.data[2]) << 16) |
            (static_cast<int32_t>(view.data[3]) << 24);
        return docLength >= 0 && static_cast<size_t>(docLength) <= view.len;
    };
    tassert(10596405, "Extension returned invalid bson obj", isValidObj(view));
    return BSONObj(reinterpret_cast<const char*>(view.data));
}

inline MongoExtensionByteView objAsByteView(const BSONObj& obj) {
    return MongoExtensionByteView{reinterpret_cast<const uint8_t*>(obj.objdata()),
                                  static_cast<size_t>(obj.objsize())};
}

inline MongoExtensionByteView stringViewAsByteView(std::string_view str) {
    return MongoExtensionByteView{reinterpret_cast<const uint8_t*>(str.data()), str.size()};
}
}  // namespace mongo::extension::sdk
