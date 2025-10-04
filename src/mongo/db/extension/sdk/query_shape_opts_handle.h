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
#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/extension_status.h"
#include "mongo/db/extension/sdk/handle.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>


namespace mongo::extension::sdk {

/**
 * Wrapper for ::MongoHostQueryShapeOpts, providing safe access to its public API through the
 * underlying vtable.
 *
 * This is an unowned handle, meaning the object is fully owned by the host, and
 * ownership is never transferred to the extension.
 */
class QueryShapeOptsHandle : public sdk::UnownedHandle<const ::MongoHostQueryShapeOpts> {
public:
    QueryShapeOptsHandle(const ::MongoHostQueryShapeOpts* ctx)
        : sdk::UnownedHandle<const ::MongoHostQueryShapeOpts>(ctx) {}

    std::string serializeIdentifier(const std::string& ident) const {
        assertValid();

        ::MongoExtensionByteBuf* buf;
        const auto& vtbl = vtable();
        auto* ptr = get();
        auto identView = sdk::stringViewAsByteView(ident);

        extension::sdk::enterC([&]() { return vtbl.serialize_identifier(ptr, &identView, &buf); });

        if (!buf) {
            // TODO SERVER-111882 tassert here instead of returning empty string, since this would
            // indicate programmer error. The implementation of serialize_identifier cannot return
            // nullptr.
            return "";
        }

        // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
        // into a string to be returned.
        sdk::VecByteBufHandle ownedBuf{static_cast<sdk::VecByteBuf*>(buf)};
        return std::string(sdk::byteViewAsStringView(ownedBuf.getByteView()));
    }

private:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(11136800,
                "HostQueryShapeOpts' 'serializeIdentifier' is null",
                vtable.serialize_identifier != nullptr);
    };
};

}  // namespace mongo::extension::sdk
