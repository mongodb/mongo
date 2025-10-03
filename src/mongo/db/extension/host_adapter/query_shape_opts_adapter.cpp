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
#include "mongo/db/extension/host_adapter/query_shape_opts_adapter.h"

#include "mongo/db/extension/sdk/byte_buf.h"
#include "mongo/db/extension/sdk/byte_buf_utils.h"
#include "mongo/db/extension/sdk/extension_status.h"

namespace mongo::extension::host {

MongoExtensionStatus* QueryShapeOptsAdapter::_extSerializeIdentifier(
    const ::MongoHostQueryShapeOpts* ctx,
    const ::MongoExtensionByteView* ident,
    ::MongoExtensionByteBuf** output) noexcept {
    return sdk::enterCXX([&]() {
        *output = nullptr;

        const auto& impl = static_cast<const QueryShapeOptsAdapter*>(ctx)->getImpl();
        auto transformedIdent =
            impl->serializeIdentifier(std::string(sdk::byteViewAsStringView(*ident)));

        // Allocate a buffer on the heap. Ownership is transferred to the caller.
        *output = new sdk::VecByteBuf(reinterpret_cast<uint8_t*>(transformedIdent.data()),
                                      transformedIdent.length());
    });
}


std::string QueryShapeOptsAdapter::serializeIdentifier(const std::string& ident) {
    return _opts->serializeIdentifier(ident);
}

}  // namespace mongo::extension::host
