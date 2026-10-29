// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * The data format of a RecordStore's RecordId keys.
 */
enum class KeyFormat {
    /** Signed 64-bit integer */
    Long,
    /** Variable-length binary comparable data */
    String,
};
}  // namespace mongo
