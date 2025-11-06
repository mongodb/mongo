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
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/util/modules.h"

namespace mongo::extension::host_connector {

class HostOperationMetricsHandle : public OwnedHandle<::MongoExtensionOperationMetrics> {
public:
    HostOperationMetricsHandle(::MongoExtensionOperationMetrics* ctx)
        : OwnedHandle<::MongoExtensionOperationMetrics>(ctx) {
        _assertValidVTable();
    }

    // Call the underlying object's serialize function and return the resulting BSON object.
    BSONObj serialize() const {
        assertValid();

        ::MongoExtensionByteBuf* buf;
        auto* ptr = get();

        invokeCAndConvertStatusToException([&]() { return vtable().serialize(ptr, &buf); });

        tassert(
            11265503, "buffer returned from serialize function must not be null", buf != nullptr);

        // Take ownership of the returned buffer so that it gets cleaned up, then copy the memory
        // into a BSON object to be returned.
        ExtensionByteBufHandle ownedBuf{buf};
        return bsonObjFromByteView(ownedBuf.getByteView()).getOwned();
    }

    // Note that we purposefully do not implement `update` on this handle - the host handle should
    // not be calling `update`. `update` should instead be called from within the extension, using
    // an ExtensionOperationMetricsHandle.

private:
    void _assertVTableConstraints(const VTable_t& vtable) const override {
        tassert(11265504, "HostOperationMetrics' 'serialize' is null", vtable.serialize != nullptr);
    };
};

}  // namespace mongo::extension::host_connector
