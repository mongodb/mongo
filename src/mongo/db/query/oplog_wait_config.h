/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

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
