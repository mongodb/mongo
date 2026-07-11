// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo::otel {
namespace [[MONGO_MOD_PUBLIC]] traces {

#ifdef MONGO_CONFIG_OTEL

Status initialize(std::string name);
void shutdown();

#else

inline Status initialize(std::string) {
    return Status::OK();
}

inline void shutdown() {}

#endif

}  // namespace traces
}  // namespace mongo::otel
