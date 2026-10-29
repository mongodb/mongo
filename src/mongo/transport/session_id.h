// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] transport {

using SessionId = uint64_t;

}  // namespace transport
}  // namespace mongo
