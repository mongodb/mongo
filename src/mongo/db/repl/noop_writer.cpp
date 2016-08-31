/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/noop_writer.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_parameters.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

MONGO_EXPORT_SERVER_PARAMETER(writePeriodicNoops, bool, true);

const auto kMsgObj = BSON("msg"
                          << "periodic noop");

}  // namespace


/**
 *  Runs the noopWrite argument with waitTime period until its destroyed.
 */
class NoopWriter::PeriodicNoopRunner {
    MONGO_DISALLOW_COPYING(PeriodicNoopRunner);

    using NoopWriteFn = stdx::function<void(OperationContext*)>;

public:
    PeriodicNoopRunner(Seconds waitTime, NoopWriteFn noopWrite)
        : _thread([this, noopWrite, waitTime] { run(waitTime, std::move(noopWrite)); }) {}

    ~PeriodicNoopRunner() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _inShutdown = true;
        _cv.notify_all();
        lk.unlock();
        _thread.join();
    }

private:
    void run(Seconds waitTime, NoopWriteFn noopWrite) {
        Client::initThread("NoopWriter");
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;
        while (true) {
            {
                stdx::unique_lock<stdx::mutex> lk(_mutex);
                _cv.wait_for(lk, waitTime.toSystemDuration(), [&] { return _inShutdown; });

                if (_inShutdown)
                    return;
            }
            noopWrite(&txn);
        }
    }

    /**
     *  Indicator that thread is shutting down.
     */
    bool _inShutdown{false};

    /**
     *  Mutex for the CV
     */
    stdx::mutex _mutex;

    /**
     * CV to wait for.
     */
    stdx::condition_variable _cv;

    /**
     * Thread that runs the tasks. Must be last so all other members are initialized before
     * starting.
     */
    stdx::thread _thread;
};

NoopWriter::NoopWriter(Seconds writeInterval) : _writeInterval(writeInterval) {
    uassert(ErrorCodes::BadValue, "write interval must be positive", writeInterval > Seconds(0));
}

NoopWriter::~NoopWriter() {
    stopWritingPeriodicNoops();
}

Status NoopWriter::startWritingPeriodicNoops(OpTime lastKnownOpTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _lastKnownOpTime = lastKnownOpTime;

    invariant(!_noopRunner);
    _noopRunner = stdx::make_unique<PeriodicNoopRunner>(
        _writeInterval, [this](OperationContext* txn) { _writeNoop(txn); });
    return Status::OK();
}

void NoopWriter::stopWritingPeriodicNoops() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _noopRunner.reset();
}

void NoopWriter::_writeNoop(OperationContext* txn) {
    ScopedTransaction transaction(txn, MODE_IX);
    // Use GlobalLock + lockMMAPV1Flush instead of DBLock to allow return when the lock is not
    // available. It may happen when the primary steps down and a shared global lock is acquired.
    Lock::GlobalLock lock(txn->lockState(), MODE_IX, 1);
    if (!lock.isLocked()) {
        LOG(1) << "Global lock is not available skipping noopWrite";
        return;
    }
    txn->lockState()->lockMMAPV1Flush();

    auto replCoord = ReplicationCoordinator::get(txn);
    // Its a proxy for being a primary
    if (!replCoord->canAcceptWritesForDatabase("admin")) {
        LOG(1) << "Not a primary, skipping the noop write";
        return;
    }

    auto lastAppliedOpTime = replCoord->getMyLastAppliedOpTime();

    // _lastKnownOpTime is not protected by lock as its used only by one thread.
    if (lastAppliedOpTime != _lastKnownOpTime) {
        LOG(1) << "Not scheduling a noop write. Last known OpTime: " << _lastKnownOpTime
               << " != last primary OpTime: " << lastAppliedOpTime;
    } else {
        if (writePeriodicNoops) {
            const auto logLevel = Command::testCommandsEnabled ? 0 : 1;
            LOG(logLevel)
                << "Writing noop to oplog as there has been no writes to this replica set in over "
                << _writeInterval;
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                WriteUnitOfWork uow(txn);
                txn->getClient()->getServiceContext()->getOpObserver()->onOpMessage(txn, kMsgObj);
                uow.commit();
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "writeNoop", rsOplogName);
        }
    }

    _lastKnownOpTime = replCoord->getMyLastAppliedOpTime();
    LOG(1) << "Set last known op time to " << _lastKnownOpTime;
}

}  // namespace repl
}  // namespace mongo
