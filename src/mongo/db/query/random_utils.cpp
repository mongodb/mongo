// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/random_utils.h"

namespace mongo::random_utils {

PseudoRandom& getRNG() {
    static thread_local PseudoRandom threadLocalRNG(SecureRandom().nextInt64());
    return threadLocalRNG;
}

}  // namespace mongo::random_utils
