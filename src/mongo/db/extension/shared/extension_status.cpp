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

#include "mongo/db/extension/shared/extension_status.h"

#include "mongo/db/extension/shared/byte_buf.h"

namespace mongo::extension {

size_t ExtensionStatusOK::sInstanceCount = 0;

size_t ExtensionStatusOK::getInstanceCount() {
    return sInstanceCount;
}

const ::MongoExtensionStatusVTable extension::ExtensionStatusOK::VTABLE = {
    .destroy = &ExtensionStatusOK::_extDestroy,
    .get_code = &ExtensionStatusOK::_extGetCode,
    .get_reason = &ExtensionStatusOK::_extGetReason,
    .set_code = &ExtensionStatusOK::_extSetCode,
    .set_reason = &ExtensionStatusOK::_extSetReason,
    .clone = &ExtensionStatusOK::_extClone,
};

const ::MongoExtensionStatusVTable extension::ExtensionStatusException::VTABLE = {
    .destroy = &ExtensionStatusException::_extDestroy,
    .get_code = &ExtensionStatusException::_extGetCode,
    .get_reason = &ExtensionStatusException::_extGetReason,
    .set_code = &ExtensionStatusException::_extSetCode,
    .set_reason = &ExtensionStatusException::_extSetReason,
    .clone = &ExtensionStatusException::_extClone,
};

const ::MongoExtensionStatusVTable extension::ExtensionGenericStatus::VTABLE = {
    .destroy = &ExtensionGenericStatus::_extDestroy,
    .get_code = &ExtensionGenericStatus::_extGetCode,
    .get_reason = &ExtensionGenericStatus::_extGetReason,
    .set_code = &ExtensionGenericStatus::_extSetCode,
    .set_reason = &ExtensionGenericStatus::_extSetReason,
    .clone = &ExtensionGenericStatus::_extClone,
};

void convertStatusToException(StatusHandle status) {
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

void StatusHandle::setCode(int code) {
    assertValid();
    vtable().set_code(get(), code);
}

void StatusHandle::setReason(const std::string& reason) {
    assertValid();
    auto byteView = stringViewAsByteView(reason);
    invokeCAndConvertStatusToException([&]() { return vtable().set_reason(get(), byteView); });
}

StatusHandle StatusHandle::clone() const {
    assertValid();
    ::MongoExtensionStatus* cloneTarget{nullptr};
    invokeCAndConvertStatusToException([&]() { return vtable().clone(get(), &cloneTarget); });
    return StatusHandle(cloneTarget);
}

}  // namespace mongo::extension
