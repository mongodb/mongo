// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"

#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace mongo::extension {

BSONObj CommandDescriptorAPI::getProperties() const {
    ::MongoExtensionByteBuf* buf{nullptr};

    invokeCAndConvertStatusToException([&]() { return _vtable().get_properties(get(), &buf); });

    tassert(ErrorCodes::ExtensionSerializationError,
            "buffer returned from get_properties must not be null",
            buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

void CommandDescriptorAPI::run(const BSONObj& requestBson,
                               const ::MongoExtensionCommandContext& ctx,
                               BSONObjBuilder& result) const {
    ::MongoExtensionByteBuf* buf{nullptr};

    invokeCAndConvertStatusToException(
        [&]() { return _vtable().run(get(), objAsByteView(requestBson), &ctx, &buf); });

    tassert(ErrorCodes::ExtensionSerializationError,
            "buffer returned from run must not be null",
            buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, and append the response
    // while the buffer is still alive to avoid an intermediate owned copy.
    ExtensionByteBufHandle ownedBuf{buf};
    result.appendElements(bsonObjFromByteView(ownedBuf->getByteView()));
}

}  // namespace mongo::extension
