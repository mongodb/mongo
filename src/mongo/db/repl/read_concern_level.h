// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/read_concern_gen.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {

using ReadConcernLevel = ReadConcernLevelEnum;

namespace readConcernLevels {

std::string_view toString(ReadConcernLevel level);

}  // namespace readConcernLevels

}  // namespace repl
}  // namespace mongo
