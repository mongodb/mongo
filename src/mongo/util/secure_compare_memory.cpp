// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/secure_compare_memory.h"

namespace mongo {

bool consttimeMemEqual(volatile const unsigned char* s1,  // NOLINT - using volatile to
                       volatile const unsigned char* s2,  // NOLINT - disable compiler optimizations
                       size_t length) {
    unsigned int ret = 0;

    for (size_t i = 0; i < length; ++i) {
        ret |= s1[i] ^ s2[i];
    }

    return (1 & ((ret - 1) >> 8));
}

}  // namespace mongo
