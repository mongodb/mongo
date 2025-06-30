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

#include "mongo/db/extension/sdk/extension_status.h"

#include "mongo/db/extension/sdk/byte_buf.h"

namespace mongo::extension::sdk {

const ::MongoExtensionStatusVTable extension::sdk::ExtensionStatus::VTABLE = {
    &ExtensionStatus::_extDestroy,
    &ExtensionStatus::_extGetCode,
    &ExtensionStatus::_extGetReason,
};

size_t ExtensionStatusOK::sInstanceCount = 0;

size_t ExtensionStatusOK::getInstanceCount() {
    return sInstanceCount;
}

const ::MongoExtensionStatusVTable extension::sdk::ExtensionStatusOK::VTABLE = {
    &ExtensionStatusOK::_extDestroy,
    &ExtensionStatusOK::_extGetCode,
    &ExtensionStatusOK::_extGetReason,
};

const ::MongoExtensionStatusVTable extension::sdk::ExtensionStatusException::VTABLE = {
    &ExtensionStatusException::_extDestroy,
    &ExtensionStatusException::_extGetCode,
    &ExtensionStatusException::_extGetReason,
};

void enterC_ErrorHandler(HostStatusHandle status) {
    /**
     * If we can extract an exception pointer from the received status from the C API call, it means
     * that this was originally a C++ exception. In that case, we can go ahead and rethrow that
     * exception here.
     *
     * Otherwise, we received an error from the extension that was not originally from this
     * execution context, so we throw our ExtensionDBException which will allow us to rethrow this
     * exception later if we happen to cross the API boundary again.
     */
    if (auto exceptionPtr = ExtensionStatusException::extractException(*status.get());
        exceptionPtr) {
        std::rethrow_exception(std::move(exceptionPtr));
    }
    throw ExtensionDBException(std::move(status));
}
}  // namespace mongo::extension::sdk
