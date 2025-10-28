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
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension::sdk {

class ExtensionOperationMetricsHandle : public UnownedHandle<::MongoExtensionOperationMetrics> {
public:
    ExtensionOperationMetricsHandle(::MongoExtensionOperationMetrics* ctx)
        : UnownedHandle<::MongoExtensionOperationMetrics>(ctx) {
        _assertValidVTable();
    }

    void update(const MongoExtensionByteView& byteView) {
        // Note that it is safe to call this without invokeC... because this is not crossing the API
        // boundary - this is called from extension side -> extension side.
        vtable().update(get(), byteView);
    }

    /**
     * It is unlikely that an extension should be calling serialize on its own operation metrics.
     * However, it might be useful for some implementations of `update` to be able to operate on
     * BSON, so we provide this functionality anyways.
     */
    BSONObj serialize() {
        assertValid();

        ::MongoExtensionByteBuf* buf;
        auto* ptr = get();

        invokeCAndConvertStatusToException([&]() { return vtable().serialize(ptr, &buf); });

        sdk_tassert(
            11265502, "buffer returned from serialize function must not be null", buf != nullptr);

        // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
        // into a BSON object to be returned.
        ExtensionByteBufHandle ownedBuf{buf};
        return bsonObjFromByteView(ownedBuf.getByteView()).getOwned();
    }

private:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        sdk_tassert(11265500,
                    "ExtensionOperationMetrics' 'serialize' is null",
                    vtable.serialize != nullptr);
        sdk_tassert(
            11265501, "ExtensionOperationMetrics' 'update' is null", vtable.update != nullptr);
    };
};


}  // namespace mongo::extension::sdk
