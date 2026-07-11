// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/service_context.h"
#include "mongo/tools/workload_simulation/event_queue.h"
#include "mongo/tools/workload_simulation/workload_characteristics.h"
#include "mongo/util/modules.h"

#include <thread>

namespace mongo::workload_simulation {

/**
 * The driver class implements the threading logic to have a number of readers and writers
 * consistently acquiring tickets and performing read and write operations under the ticket.
 * Currently all operations are performed with 'kNormal' priority. The driver uses a
 * 'MockWorkloadCharacteristics' instance to determine appropriate latencies for read and write
 * operations at the current concurrency levels, and simply 'sleeps' (using the queue) for those
 * desired latencies instead of performing real work. This should result in ticket holder throughput
 * of approximately that targeted by the workload characteristics.
 */
class TicketedWorkloadDriver {
public:
    TicketedWorkloadDriver(EventQueue& queue,
                           std::unique_ptr<MockWorkloadCharacteristics>&& characteristics);
    virtual ~TicketedWorkloadDriver();

    /**
     * Initializes 'numReaders' read and 'numWriters' write actor threads, which immediately begin
     * filing work with the 'EventQueue'.
     */
    void start(ServiceContext* svcCtx,
               TicketHolder* readTicketHolder,
               TicketHolder* writeTicketHolder,
               int32_t numReaders,
               int32_t numWriters);

    /**
     * Resizes to 'numReaders' read and 'numWriters' write actor threads.
     */
    void resize(int32_t numReaders, int32_t numWriters);

    /**
     * Stops and joins actor threads.
     */
    void stop();

    /**
     * Reports optimal and allocated read and write ticket counts, e.g.
     * {
     *   read: { optimal: 10, allocated: 5 }
     *   write: { optimal: 10, allocated:  5 }
     * }
     */
    virtual BSONObj metrics() const;

protected:
    void _read(int32_t i);
    void _write(int32_t i);

    void _doRead(OperationContext* opCtx, AdmissionContext* admCtx);
    void _doWrite(OperationContext* opCtx, AdmissionContext* admCtx);

protected:
    mutable std::mutex _mutex;

    EventQueue& _queue;
    std::unique_ptr<MockWorkloadCharacteristics> _characteristics;

    ServiceContext* _svcCtx = nullptr;
    TicketHolder* _readTicketHolder = nullptr;
    TicketHolder* _writeTicketHolder = nullptr;
    int32_t _numReaders = 0;
    int32_t _numWriters = 0;

    std::vector<stdx::thread> _readWorkers;
    std::vector<stdx::thread> _writeWorkers;

    // The values stored in each of these variables refers to the maximum index of the respective
    // type of worker that should be active and running at the moment. I.e. a value of -1 means no
    // active workers, while 4 means 5 active workers.
    Atomic<int32_t> _readRunning{-1};
    Atomic<int32_t> _writeRunning{-1};
};

}  // namespace mongo::workload_simulation
