// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/sha256_block.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] query_shape {

using QueryShapeHash = SHA256Block;

}  // namespace query_shape
}  // namespace mongo
