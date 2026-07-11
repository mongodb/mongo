// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collator_interface.h"

#include "mongo/base/string_data_comparator.h"

#include <string_view>

namespace mongo {

void CollatorInterface::hash_combine(size_t& seed, std::string_view stringToHash) const {
    auto comparisonKey = getComparisonKey(stringToHash);
    simpleStringDataComparator.hash_combine(seed, comparisonKey.getKeyData());
}

std::string CollatorInterface::getComparisonString(std::string_view stringData) const {
    return std::string{getComparisonKey(stringData).getKeyData()};
}

}  // namespace mongo
