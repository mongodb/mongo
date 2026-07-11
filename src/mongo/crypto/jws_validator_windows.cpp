// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/crypto/jws_validator.h"

#include <string_view>

namespace mongo::crypto {

StatusWith<std::unique_ptr<JWSValidator>> JWSValidator::create(std::string_view algorithm,
                                                               const BSONObj& key) {
    return {ErrorCodes::OperationFailed, "Signature Verification Not Available"};
}
}  // namespace mongo::crypto
