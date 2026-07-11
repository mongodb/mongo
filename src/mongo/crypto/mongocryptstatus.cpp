// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/crypto/mongocryptstatus.h"

#include <fmt/format.h>
extern "C" {
#include <string_view>

#include <mongocrypt.h>
}

#include "mongo/util/assert_util.h"

namespace mongo {
using namespace std::literals::string_view_literals;

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
    std::string_view errorPrefix;
    switch (mongocrypt_status_type(_status)) {
        case MONGOCRYPT_STATUS_OK:
            return Status::OK();
        case MONGOCRYPT_STATUS_ERROR_CLIENT:
            errorPrefix = "Client Error"sv;
            break;
        case MONGOCRYPT_STATUS_ERROR_KMS:
            errorPrefix = "KMS Error"sv;
            break;
        case MONGOCRYPT_STATUS_ERROR_CRYPT_SHARED:
            errorPrefix = "Crypt Shared Error"sv;
            break;
        default:
            errorPrefix = "Unexpected Error"sv;
            break;
    }

    return Status(ErrorCodes::LibmongocryptError, fmt::format("{}: {}", errorPrefix, reason()));
}

}  // namespace mongo
