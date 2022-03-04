/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#if defined(__linux__)
#include <semaphore.h>
#endif

#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/time_support.h"

namespace mongo {

class TicketHolder {
public:
    virtual ~TicketHolder() = 0;

    /**
     * Attempts to acquire a ticket without blocking.
     * Returns a boolean indicating whether the operation was successful or not.
     */
    virtual bool tryAcquire() = 0;

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the OperationContext
     * 'opCtx' is killed, throwing an AssertionException.
     * If 'opCtx' is not provided or equal to nullptr, the wait is not interruptible.
     */
    virtual void waitForTicket(OperationContext* opCtx) = 0;
    void waitForTicket() {
        this->waitForTicket(nullptr);
    };

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the OperationContext 'opCtx' is killed and no waits for tickets can
     * proceed.
     * If 'opCtx' is not provided or equal to nullptr, the wait is not interruptible.
     */
    virtual bool waitForTicketUntil(OperationContext* opCtx, Date_t until) = 0;
    bool waitForTicketUntil(Date_t until) {
        return this->waitForTicketUntil(nullptr, until);
    };

    virtual void release() = 0;

    virtual Status resize(int newSize) = 0;

    virtual int available() const = 0;

    virtual int used() const = 0;

    virtual int outof() const = 0;
};

class SemaphoreTicketHolder final : public TicketHolder {
public:
    explicit SemaphoreTicketHolder(int num);
    ~SemaphoreTicketHolder() override final;

    bool tryAcquire() override final;

    void waitForTicket(OperationContext* opCtx) override final;

    bool waitForTicketUntil(OperationContext* opCtx, Date_t until) override final;

    void release() override final;

    Status resize(int newSize) override final;

    int available() const override final;

    int used() const override final;

    int outof() const override final;

private:
#if defined(__linux__)
    mutable sem_t _sem;

    // You can read _outof without a lock, but have to hold _resizeMutex to change.
    AtomicWord<int> _outof;
    Mutex _resizeMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "SemaphoreTicketHolder::_resizeMutex");
#else
    bool _tryAcquire();

    AtomicWord<int> _outof;
    int _num;
    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "SemaphoreTicketHolder::_mutex");
    stdx::condition_variable _newTicket;
#endif
};

class FifoTicketHolder final : public TicketHolder {
public:
    explicit FifoTicketHolder(int num);
    ~FifoTicketHolder() override final;

    bool tryAcquire() override final;

    void waitForTicket(OperationContext* opCtx) override final;

    bool waitForTicketUntil(OperationContext* opCtx, Date_t until) override final;

    void release() override final;

    Status resize(int newSize) override final;

    int available() const override final;

    int used() const override final;

    int outof() const override final;

private:
    void _release(WithLock);

    Mutex _resizeMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "FifoTicketHolder::_resizeMutex");
    AtomicWord<int> _capacity;
    AtomicWord<int> _numAvailable;
    AtomicWord<int> _elementsInQueue;
    enum class WaitingState { Waiting, Cancelled, Assigned };
    struct WaitingElement {
        stdx::condition_variable signaler;
        Mutex modificationMutex = MONGO_MAKE_LATCH(
            HierarchicalAcquisitionLevel(1), "FifoTicketHolder::WaitingElement::modificationMutex");
        WaitingState state;
    };
    MultiProducerMultiConsumerQueue<std::shared_ptr<WaitingElement>> _queue;
    // _queueMutex protects all modifications made to either the _queue, or the statistics of the
    // queue.
    Mutex _queueMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "FifoTicketHolder::_queueMutex");
};

class ScopedTicket {
public:
    ScopedTicket(TicketHolder* holder) : _holder(holder) {
        _holder->waitForTicket();
    }

    ~ScopedTicket() {
        _holder->release();
    }

private:
    TicketHolder* _holder;
};

class TicketHolderReleaser {
    TicketHolderReleaser(const TicketHolderReleaser&) = delete;
    TicketHolderReleaser& operator=(const TicketHolderReleaser&) = delete;

public:
    TicketHolderReleaser() {
        _holder = nullptr;
    }

    explicit TicketHolderReleaser(TicketHolder* holder) {
        _holder = holder;
    }

    ~TicketHolderReleaser() {
        if (_holder) {
            _holder->release();
        }
    }

    bool hasTicket() const {
        return _holder != nullptr;
    }

    void reset(TicketHolder* holder = nullptr) {
        if (_holder) {
            _holder->release();
        }
        _holder = holder;
    }

private:
    TicketHolder* _holder;
};
}  // namespace mongo
