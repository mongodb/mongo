// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/throughput_probing/ticketed_workload_driver.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
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
    std::lock_guard lk{_mutex};

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
    std::lock_guard lk{_mutex};

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
    std::lock_guard lk{_mutex};

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
    std::lock_guard lk{_mutex};
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

    while (_readRunning.load() >= i) {
        auto& admCtx = ExecutionAdmissionContext::get(opCtx.get());
        Ticket ticket = _readTicketHolder->waitForTicket(opCtx.get(), &admCtx);
        _doRead(opCtx.get(), &admCtx);
    }
}

void TicketedWorkloadDriver::_write(int32_t i) {
    auto client = _svcCtx->getService()->makeClient("writer_" + std::to_string(i));
    auto opCtx = client->makeOperationContext();

    while (_writeRunning.load() >= i) {
        auto& admCtx = ExecutionAdmissionContext::get(opCtx.get());
        Ticket ticket = _writeTicketHolder->waitForTicket(opCtx.get(), &admCtx);
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
