/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/timestamp.h"

namespace mongo::repl {

/**
 * Observer interface for opTime advances. Implementations must be fast (a single atomic store is
 * ideal).
 *
 * Callbacks are invoked from a dedicated dispatcher thread outside of any replication lock.
 * Observers are registered once before startup and are never removed.
 */
class OpTimeObserver {
public:
    virtual ~OpTimeObserver() = default;

    /**
     * Called whenever the observed opTime advances to a new non-null value.
     *
     * NOTE: Intermediate values may be skipped. If the opTime advances multiple times before this
     * observer can be notified, only the most recent value will be delivered. Implementations must
     * not assume they will observe every opTime. The only guarantee is that this method will
     * eventually be called with the most recently applied opTime.
     */
    virtual void onOpTime(const Timestamp& ts) = 0;
};

}  // namespace mongo::repl
