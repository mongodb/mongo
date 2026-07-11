// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

/**
 * Helpers for converting between MongoExtensionByteView and host types (BSONObj, string_view,
 * std::string_view) and for creating empty byte containers. Used on both sides of the extension
 * boundary.
 */
namespace mongo::extension {

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
    tassert(ErrorCodes::ExtensionSerializationError,
            "Extension returned invalid bson obj",
            isValidObj(view));
    return BSONObj(reinterpret_cast<const char*>(view.data));
}

inline MongoExtensionByteView objAsByteView(const BSONObj& obj) {
    return MongoExtensionByteView{reinterpret_cast<const uint8_t*>(obj.objdata()),
                                  static_cast<size_t>(obj.objsize())};
}

inline MongoExtensionByteView stringViewAsByteView(std::string_view str) {
    return MongoExtensionByteView{reinterpret_cast<const uint8_t*>(str.data()), str.size()};
}

inline MongoExtensionByteView stringDataAsByteView(std::string_view sd) noexcept {
    return MongoExtensionByteView{reinterpret_cast<const uint8_t*>(sd.data()),
                                  static_cast<size_t>(sd.size())};
}

inline ::MongoExtensionByteContainer createEmptyByteContainer() {
    return {MongoExtensionByteContainerType::kByteView, MongoExtensionByteView{nullptr, 0}};
};
}  // namespace mongo::extension
