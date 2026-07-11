// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/database_name.h"

#include <string_view>

#include <boost/container_hash/hash.hpp>

namespace mongo {
size_t hash_value(const DatabaseName& dbn) {
    return boost::hash<std::string_view>{}(
        dbn.view().substr(0, dbn.sizeWithTenant() + DatabaseName::kDataOffset));
}
}  // namespace mongo
