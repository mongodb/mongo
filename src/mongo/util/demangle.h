// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <typeinfo>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

std::string demangleName(const std::type_info& typeinfo);

}  // namespace mongo
