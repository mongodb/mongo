/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/mongocryptstatus.h"

#include <fmt/format.h>
extern "C" {
#include <mongocrypt.h>
}

#include "mongo/util/assert_util.h"

namespace mongo {

MongoCryptStatus::MongoCryptStatus() {
    _status = mongocrypt_status_new();
    uassert(
        ErrorCodes::OperationFailed, "Allocation failure creating mongocrypt_status_t", _status);
}

MongoCryptStatus::~MongoCryptStatus() {
    mongocrypt_status_destroy(_status);
}

uint32_t MongoCryptStatus::getCode() const {
    return mongocrypt_status_code(_status);
}

bool MongoCryptStatus::isOK() const {
    return mongocrypt_status_ok(_status);
}

std::string MongoCryptStatus::reason() const {
    uint32_t len = 0;
    const char* msg = mongocrypt_status_message(_status, &len);
    if (!msg) {
        return std::string();
    }
    return {msg, len};
}

Status MongoCryptStatus::toStatus() const {
    StringData errorPrefix;
    switch (mongocrypt_status_type(_status)) {
        case MONGOCRYPT_STATUS_OK:
            return Status::OK();
        case MONGOCRYPT_STATUS_ERROR_CLIENT:
            errorPrefix = "Client Error"_sd;
            break;
        case MONGOCRYPT_STATUS_ERROR_KMS:
            errorPrefix = "KMS Error"_sd;
            break;
        case MONGOCRYPT_STATUS_ERROR_CRYPT_SHARED:
            errorPrefix = "Crypt Shared Error"_sd;
            break;
        default:
            errorPrefix = "Unexpected Error"_sd;
            break;
    }

    return Status(ErrorCodes::LibmongocryptError, fmt::format("{}: {}", errorPrefix, reason()));
}

}  // namespace mongo
