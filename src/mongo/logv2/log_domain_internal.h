// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_source.h"
#include "mongo/util/modules.h"

namespace mongo::logv2 {

class LogDomain::Internal {
public:
    Internal() = default;
    virtual ~Internal();

    virtual LogSource& source() = 0;
};

}  // namespace mongo::logv2
