// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <string>


namespace mongo {

Status validateTrafficRecordDestination(const std::string& path);


}  // namespace mongo
