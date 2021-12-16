/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include <cstdint>
#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sorter/options.h"
#include "mongo/db/storage/encryption_hooks.h"

namespace mongo::sorter {
/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number. Each name is suffixed with a random number generated at startup, to prevent name
 * collisions when the external sort files are preserved across restarts.
 */
std::string nextFileName(StringData name);

/**
 * Calculates and returns a new murmur hash value based on the prior murmur hash and a new piece
 * of data.
 */
uint32_t addDataToChecksum(const void* data, std::size_t size, uint32_t checksum);

/**
 * Returns the current EncryptionHooks registered with the global service context.
 * Returns nullptr if the service context is not available; or if the EncyptionHooks
 * registered is not enabled.
 */
EncryptionHooks* getEncryptionHooksIfEnabled();

template <typename Data>
void dassertCompIsSane(const std::function<int(const Data& lhs, const Data& rhs)>& comp,
                       const Data& lhs,
                       const Data& rhs) {
#if defined(MONGO_CONFIG_DEBUG_BUILD) && !defined(_MSC_VER)
    // MSVC++ already does similar verification in debug mode in addition to using algorithms that
    // do more comparisons. Doing our own verification in addition makes debug builds considerably
    // slower without any additional safety.

    // Test reversed comparisons.
    const int regular = comp(lhs, rhs);
    if (regular == 0) {
        invariant(comp(rhs, lhs) == 0);
    } else if (regular < 0) {
        invariant(comp(rhs, lhs) > 0);
    } else {
        invariant(comp(rhs, lhs) < 0);
    }

    // Test reflexivity.
    invariant(comp(lhs, lhs) == 0);
    invariant(comp(rhs, rhs) == 0);
#endif
}
}  // namespace mongo::sorter
