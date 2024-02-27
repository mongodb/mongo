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

#include "mongo/tools/workload_simulation/throughput_probing/ticketed_workload_driver.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"

namespace mongo::workload_simulation {


TicketedWorkloadDriver::TicketedWorkloadDriver(
    EventQueue& queue, std::unique_ptr<MockWorkloadCharacteristics>&& characteristics)
    : _queue{queue}, _characteristics{std::move(characteristics)} {}

TicketedWorkloadDriver::~TicketedWorkloadDriver() {
    if (!_readWorkers.empty() || !_writeWorkers.empty()) {
        stop();
    }
}

void TicketedWorkloadDriver::start(ServiceContext* svcCtx,
                                   TicketHolder* readTicketHolder,
                                   TicketHolder* writeTicketHolder,
                                   int32_t numReaders,
                                   int32_t numWriters) {
    stdx::lock_guard lk{_mutex};

    _svcCtx = svcCtx;
    _readTicketHolder = readTicketHolder;
    _writeTicketHolder = writeTicketHolder;
    _numReaders = numReaders;
    _numWriters = numWriters;

    for (int32_t i = 0; i < _numReaders; ++i) {
        _readRunning.fetchAndAdd(1);
        _readWorkers.emplace_back([this, i]() { _read(i); });
    }

    for (int32_t i = 0; i < _numWriters; ++i) {
        _writeRunning.fetchAndAdd(1);
        _writeWorkers.emplace_back([this, i]() { _write(i); });
    }
}

void TicketedWorkloadDriver::resize(int32_t numReaders, int32_t numWriters) {
    stdx::lock_guard lk{_mutex};

    invariant(numReaders > 0);
    invariant(numWriters > 0);

    if (numReaders > _numReaders) {
        for (int32_t i = _numReaders; i < numReaders; ++i) {
            _readRunning.fetchAndAdd(1);
            _readWorkers.emplace_back([this, i]() { _read(i); });
        }
    } else if (numReaders < _numReaders) {
        for (int32_t i = _numReaders - 1; i >= numReaders; --i) {
            _readRunning.fetchAndSubtract(1);
            _readWorkers[i].join();
            _readWorkers.pop_back();
        }
    }

    if (numWriters > _numWriters) {
        for (int32_t i = _numWriters; i < numWriters; ++i) {
            _writeRunning.fetchAndAdd(1);
            _writeWorkers.emplace_back([this, i]() { _write(i); });
        }
    } else if (numWriters < _numWriters) {
        for (int32_t i = _numWriters - 1; i >= numWriters; --i) {
            _writeRunning.fetchAndSubtract(1);
            _writeWorkers[i].join();
            _writeWorkers.pop_back();
        }
    }

    _numReaders = numReaders;
    _numWriters = numWriters;
}


void TicketedWorkloadDriver::stop() {
    stdx::lock_guard lk{_mutex};

    // Request all threads stop asynchronously
    _readRunning.store(-1);
    _writeRunning.store(-1);

    // Join threads
    for (auto&& worker : _readWorkers) {
        worker.join();
    }
    for (auto&& worker : _writeWorkers) {
        worker.join();
    }

    _readWorkers.clear();
    _writeWorkers.clear();

    _numReaders = 0;
    _numWriters = 0;
    _readTicketHolder = nullptr;
    _writeTicketHolder = nullptr;
    _svcCtx = nullptr;
}

BSONObj TicketedWorkloadDriver::metrics() const {
    stdx::lock_guard lk{_mutex};
    BSONObjBuilder builder;

    {
        BSONObjBuilder read{builder.subobjStart("read")};
        read.appendNumber("optimal", _characteristics->optimal().read);
        read.appendNumber("allocated", _readTicketHolder->outof());
        read.appendNumber("provided", static_cast<int32_t>(_readWorkers.size()));
    }

    {
        BSONObjBuilder write{builder.subobjStart("write")};
        write.appendNumber("optimal", _characteristics->optimal().write);
        write.appendNumber("allocated", _writeTicketHolder->outof());
        write.appendNumber("provided", static_cast<int32_t>(_writeWorkers.size()));
    }

    return builder.obj();
}

void TicketedWorkloadDriver::_read(int32_t i) {
    auto client = _svcCtx->getService()->makeClient("reader_" + std::to_string(i));
    auto opCtx = client->makeOperationContext();
    Microseconds timeInQueue;

    while (_readRunning.load() >= i) {
        auto admCtx = AdmissionContext::get(opCtx.get());
        Ticket ticket = _readTicketHolder->waitForTicket(*opCtx, &admCtx, timeInQueue);
        _doRead(opCtx.get(), &admCtx);
    }
}

void TicketedWorkloadDriver::_write(int32_t i) {
    auto client = _svcCtx->getService()->makeClient("writer_" + std::to_string(i));
    auto opCtx = client->makeOperationContext();
    Microseconds timeInQueue;

    while (_writeRunning.load() >= i) {
        auto admCtx = AdmissionContext::get(opCtx.get());
        Ticket ticket = _writeTicketHolder->waitForTicket(*opCtx, &admCtx, timeInQueue);
        _doWrite(opCtx.get(), &admCtx);
    }
}

void TicketedWorkloadDriver::_doRead(OperationContext* opCtx, AdmissionContext* admCtx) {
    auto latency =
        _characteristics->readLatency({_readTicketHolder->used(), _writeTicketHolder->used()});
    _queue.wait_for(latency, EventQueue::WaitType::Event);
}

void TicketedWorkloadDriver::_doWrite(OperationContext* opCtx, AdmissionContext* admCtx) {
    auto latency =
        _characteristics->writeLatency({_readTicketHolder->used(), _writeTicketHolder->used()});
    _queue.wait_for(latency, EventQueue::WaitType::Event);
}

}  // namespace mongo::workload_simulation
