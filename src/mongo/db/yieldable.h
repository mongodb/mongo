// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
class [[MONGO_MOD_OPEN]] Yieldable {
public:
    virtual ~Yieldable() {}
    virtual bool yieldable() const = 0;
    virtual void yield() const = 0;
    virtual void restore() const = 0;
};
}  // namespace mongo
