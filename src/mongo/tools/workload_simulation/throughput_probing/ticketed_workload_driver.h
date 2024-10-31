/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <thread>

#include "mongo/db/service_context.h"
#include "mongo/tools/workload_simulation/event_queue.h"
#include "mongo/tools/workload_simulation/workload_characteristics.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"

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
    mutable stdx::mutex _mutex;

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
    AtomicWord<int32_t> _readRunning{-1};
    AtomicWord<int32_t> _writeRunning{-1};
};

}  // namespace mongo::workload_simulation
