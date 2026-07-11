// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {


/**
 * Once scheduled, the NoopWriter will periodically write a noop to the oplog if the replication
 * coordinator's optime has not changed since the last time it did a write.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] NoopWriter {
    NoopWriter(const NoopWriter&) = delete;
    NoopWriter& operator=(const NoopWriter&) = delete;

public:
    NoopWriter(Seconds waitTime);

    ~NoopWriter();

    Status startWritingPeriodicNoops(OpTime lastKnownOpTime);

    void stopWritingPeriodicNoops();

private:
    class PeriodicNoopRunner;

    void _writeNoop(OperationContext*);

    /**
     * NoopWriter will write a noop to the oplog every _writeInterval seconds.
     */
    const Seconds _writeInterval;

    /**
     * The last optime that has been seen by the noop writer.
     */
    OpTime _lastKnownOpTime;

    /**
     * Protects member data of this class during start and stop. There is no need to synchronize
     * access once its running because its run by a one thread only.
     */
    mutable std::mutex _mutex;

    std::unique_ptr<PeriodicNoopRunner> _noopRunner;
};

}  // namespace repl
}  // namespace mongo
