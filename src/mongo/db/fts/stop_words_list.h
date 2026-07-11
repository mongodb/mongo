// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <set>
#include <string>

namespace mongo {
namespace fts {
void loadStopWordMap(StringMap<std::set<std::string>>* m);
}  // namespace fts
}  // namespace mongo
