// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Tracks whether we are allowed to wait for oplog visibility as well as whether we have waited for
 * visiblity.
 */
class OplogWaitConfig {
public:
    OplogWaitConfig() = default;

    void enableWaitingForOplogVisibility() {
        _shouldWaitForVisiblity = true;
    }

    void setWaitedForOplogVisibility() {
        tassert(
            9478712, "Cannot wait for oplog visibility if it is disabled", _shouldWaitForVisiblity);
        _waitedForOplogVisibility = true;
    }
    bool shouldWaitForOplogVisibility() const {
        return _shouldWaitForVisiblity && !_waitedForOplogVisibility;
    }

    bool waitedForOplogVisiblity() const {
        if (_waitedForOplogVisibility) {
            tassert(9478715,
                    "Cannot wait for oplog visibility if it is disabled",
                    _shouldWaitForVisiblity);
        }
        return _waitedForOplogVisibility;
    }

private:
    // Tracks whether we should wait for oplog visiblity at all.
    bool _shouldWaitForVisiblity = false;

    // Tracks whether we have waited for oplog visiblity.
    bool _waitedForOplogVisibility = false;
};
}  // namespace mongo
