/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/encryption_hooks.h"

#include <boost/filesystem/path.hpp>

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/data_protector.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

struct EncryptionHooksHolder {
    std::unique_ptr<EncryptionHooks> ptr = std::make_unique<EncryptionHooks>();
};

const auto getEncryptionHooks = ServiceContext::declareDecoration<EncryptionHooksHolder>();
}  // namespace

void EncryptionHooks::set(ServiceContext* service, std::unique_ptr<EncryptionHooks> custHooks) {
    getEncryptionHooks(service).ptr = std::move(custHooks);
}

EncryptionHooks* EncryptionHooks::get(ServiceContext* service) {
    return getEncryptionHooks(service).ptr.get();
}

EncryptionHooks::~EncryptionHooks() {}

bool EncryptionHooks::enabled() const {
    return false;
}

bool EncryptionHooks::restartRequired() {
    return false;
}

std::unique_ptr<DataProtector> EncryptionHooks::getDataProtector() {
    return std::unique_ptr<DataProtector>();
}

boost::filesystem::path EncryptionHooks::getProtectedPathSuffix() {
    return "";
}

Status EncryptionHooks::protectTmpData(
    const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) {
    return Status(ErrorCodes::InternalError,
                  "Encryption hooks must be enabled to use preprocessTmpData.");
}

Status EncryptionHooks::unprotectTmpData(
    const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) {
    return Status(ErrorCodes::InternalError,
                  "Encryption hooks must be enabled to use postprocessTmpData.");
}

StatusWith<std::vector<std::string>> EncryptionHooks::beginNonBlockingBackup() {
    return std::vector<std::string>();
}

Status EncryptionHooks::endNonBlockingBackup() {
    return Status::OK();
}
}  // namespace mongo
