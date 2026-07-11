// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
class [[MONGO_MOD_PUBLIC]] CriticalSectionSignal {
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
