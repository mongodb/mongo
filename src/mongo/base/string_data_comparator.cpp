// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/string_data_comparator.h"

#include "mongo/util/murmur3.h"

#include <cstdlib>
#include <string_view>

namespace mongo {

void SimpleStringDataComparator::hash_combine(size_t& seed, std::string_view stringToHash) const {
    seed = murmur3<sizeof(seed)>(stringToHash, seed);
}

}  // namespace mongo
