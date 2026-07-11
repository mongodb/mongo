// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <memory>

#include <openssl/dh.h>

namespace mongo {
struct DHFreer {
    void operator()(DH* const dh) noexcept {
        if (dh) {
            ::DH_free(dh);
        }
    }
};

using UniqueDHParams = std::unique_ptr<DH, DHFreer>;

UniqueDHParams makeDefaultDHParameters();

int verifyDHParameters(const UniqueDHParams& dhparams);

}  // namespace mongo
