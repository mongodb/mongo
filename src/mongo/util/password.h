// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/modules.h"

#include <string>

#include <boost/program_options.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

std::string askPassword();
}  // namespace mongo
