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

#include "mongo/db/extension/sdk/query_shape_opts_handle.h"

namespace mongo::extension::sdk {

std::string QueryShapeOptsHandle::serializeUsingOptsHelper(
    const std::function<MongoExtensionByteView()>& getByteViewToSerialize,
    const std::function<MongoExtensionStatus*(const MongoHostQueryShapeOpts*,
                                              const MongoExtensionByteView*,
                                              ::MongoExtensionByteBuf**)>& apiFunc) const {
    assertValid();

    ::MongoExtensionByteBuf* buf;
    auto* ptr = get();
    auto byteView = getByteViewToSerialize();

    extension::sdk::enterC([&]() { return apiFunc(ptr, &byteView, &buf); });

    if (!buf) {
        // TODO SERVER-111882 tassert here instead of returning empty string, since this would
        // indicate programmer error.
        return "";
    }

    // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
    // into a string to be returned.
    sdk::VecByteBufHandle ownedBuf{static_cast<sdk::VecByteBuf*>(buf)};
    return std::string(sdk::byteViewAsStringView(ownedBuf.getByteView()));
}


std::string QueryShapeOptsHandle::serializeIdentifier(const std::string& identifier) const {
    auto getByteView = [&identifier]() {
        return sdk::stringViewAsByteView(identifier);
    };

    return serializeUsingOptsHelper(getByteView, vtable().serialize_identifier);
}

std::string QueryShapeOptsHandle::serializeFieldPath(const std::string& fieldPath) const {
    auto getByteView = [&fieldPath]() {
        return sdk::stringViewAsByteView(fieldPath);
    };

    return serializeUsingOptsHelper(getByteView, vtable().serialize_field_path);
}

}  // namespace mongo::extension::sdk
