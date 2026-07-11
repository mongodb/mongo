// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

#include <string>
#include <vector>

namespace mongo::dbtests {

std::vector<std::string> getFrameworkSuites();

}  // namespace mongo::dbtests
