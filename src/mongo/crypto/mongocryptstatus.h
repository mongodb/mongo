// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/crypto/mongocrypt_definitions.h"
#include "mongo/util/modules.h"

#include <string>

typedef struct _mongocrypt_status_t mongocrypt_status_t;

namespace mongo {

/**
 * C++ friendly wrapper around libmongocrypt's public mongocrypt_status_t* and its associated
 * functions.
 */
class MongoCryptStatus {
public:
    MongoCryptStatus();
    ~MongoCryptStatus();

    MongoCryptStatus(MongoCryptStatus&) = delete;
    MongoCryptStatus(MongoCryptStatus&&) = default;

    /**
     * Get a libmongocrypt specific error code
     */
    uint32_t getCode() const;

    /**
     * Returns true if there are no errors
     */
    bool isOK() const;

    operator mongocrypt_status_t*() {
        return _status;
    }

    std::string reason() const;

    /**
     * Convert a mongocrypt_status_t to a mongo::Status.
     */
    Status toStatus() const;

private:
    mongocrypt_status_t* _status;
};

}  // namespace mongo
