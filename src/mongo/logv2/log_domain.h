// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::logv2 {
class LogComponentSettings;

// Log domain class, implemented with the pimpl idiom to not leak out boost::log types
class LogDomain {
public:
    class Internal;

    explicit LogDomain(std::unique_ptr<Internal> internalDomain);
    ~LogDomain();

    Internal& internal() {
        return *_internal;
    }
    const Internal& internal() const {
        return *_internal;
    }

private:
    std::unique_ptr<Internal> _internal;
};

}  // namespace mongo::logv2
