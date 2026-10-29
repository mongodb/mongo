// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo::mozjs {

/**
 * Bounds the total bytes server-side JS print() may log between scope resets, so user JS cannot
 * flood the server log. The print that crosses the limit is still logged (and truncated by logv2);
 * everything after it is dropped until reset().
 */
class [[MONGO_MOD_PUBLIC]] JSPrintBudget {
public:
    static constexpr std::size_t kLimitBytes = 64 * 1024;

    bool exhausted() const {
        return remainingBytes() == 0;
    }

    std::size_t remainingBytes() const {
        return _usedBytes >= kLimitBytes ? 0 : kLimitBytes - _usedBytes;
    }

    void consume(std::size_t bytes) {
        _usedBytes += bytes;
    }

    /**
     * Returns true only on the first call after the budget is exhausted, so the suppression
     * notice is logged once per reset.
     */
    bool shouldLogSuppression() {
        if (_suppressionLogged) {
            return false;
        }
        _suppressionLogged = true;
        return true;
    }

    void reset() {
        _usedBytes = 0;
        _suppressionLogged = false;
    }

private:
    std::size_t _usedBytes = 0;
    bool _suppressionLogged = false;
};

}  // namespace mongo::mozjs
