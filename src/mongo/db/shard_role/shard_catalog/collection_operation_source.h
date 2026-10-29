// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Enum used to differentiate between types of insert/update/delete operations based on how they
 * were issued.
 */
enum class OperationSource {
    kStandard,     // Default case, use this if none of the others applies.
    kFromMigrate,  // From a chunk migration.
    kTimeseriesInsert,
    kTimeseriesUpdate,
    kTimeseriesDelete,
};

std::string_view toString(OperationSource source);
}  // namespace mongo
