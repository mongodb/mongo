// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo::crypto {

class JWSValidator {
public:
    virtual ~JWSValidator() = default;

    /**
     * Validate the provided payload/signature pair
     * using the algorithm provided.
     * In practice, this will be:
     * validate('RS256', 'headerb64url.bodyb64url', 'signatureoctets')
     */
    virtual Status validate(std::string_view algorithm,
                            std::string_view payload,
                            std::string_view signature) const = 0;

    /**
     * Instantiate a new JWSValidator for the given
     * key type (e.g. 'RSA') and JWK key data.
     * Returns, for example, JWSValidatorOpenSSLRSA
     */
    static StatusWith<std::unique_ptr<JWSValidator>> create(std::string_view algorithm,
                                                            const BSONObj& key);
};

}  // namespace mongo::crypto
