// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo::transport {

struct [[MONGO_MOD_PUBLIC]] ReactorOptions {
    bool alwaysEmitFullHistogram = false;
};

}  // namespace mongo::transport
