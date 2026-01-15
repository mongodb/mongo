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

#include "mongo/db/operation_context.h"
#include "mongo/util/future.h"

namespace mongo {
/**
 * A CriticalSectionSignal is as its name implies a way to wait for the critical section being
 * released. It's usefulness is in synchronizing between CRUD and DDL operations trying to
 * access/modify the shard filtering data.
 *
 * Note that a each instance of the CriticalSectionSignal object will only be linked to a single
 * instance of the critical section being taken. If the critical section is taken again after
 * waiting on this signal then the CriticalSectionSignal object will not wait on the new critical
 * section being released since the original one has already been released.
 */
class MONGO_MOD_PUBLIC CriticalSectionSignal {
public:
    enum class CriticalSectionType { Database, Collection };
    CriticalSectionSignal(SharedSemiFuture<void> signal, CriticalSectionType type)
        : _signal(signal), _type(type) {}

    /**
     * Waits until the critical section has been released. Waiting here is a blocking operation and
     * should be done with great care not to hold any locks since they will block other operations
     * while waiting for the signal.
     *
     * Waiting multiple times is possible but only the first time will actually wait. Even if the
     * critical section gets acquired again this will immediately return since the signal is only
     * valid for a single instance of the critical section. The next critical section acquisition
     * will result in a new signal being created so any wait should occur on the new instance.
     */
    void get(OperationContext* opCtx) const;

private:
    SharedSemiFuture<void> _signal;
    CriticalSectionType _type;
};
}  // namespace mongo
