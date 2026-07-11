// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] procparser {

Status onUpdateProcFileSizeLimit(const long long& limit);

}  // namespace procparser
}  // namespace mongo
