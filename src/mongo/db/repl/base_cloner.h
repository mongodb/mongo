/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <functional>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/repl/initial_sync_shared_data.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

namespace mongo {
namespace repl {

class BaseCloner {
public:
    BaseCloner(StringData clonerName,
               InitialSyncSharedData* sharedData,
               HostAndPort source,
               DBClientConnection* client,
               StorageInterface* storageInterface,
               ThreadPool* dbPool);

    virtual ~BaseCloner() = default;

    /**
     * run() catches all database exceptions and stores them in _status, to simplify error
     * handling in the caller above.  It returns its own _status if that is not OK, otherwise
     * the shared initial sync status.
     */
    Status run();

    /**
     * Executes the run() method asychronously on the given taskExecutor when the event is
     * signalled, returning the result as a Future.
     *
     * If the executor is valid, the Future is guaranteed to not be ready until the event is
     * signalled.  If the executor is not valid (e.g. shutting down), the future will be
     * ready immediately after the call and the EventHandle will be invalid.
     */
    std::pair<Future<void>, executor::TaskExecutor::EventHandle> runOnExecutorEvent(
        executor::TaskExecutor* executor);

    /**
     * For unit testing, allow stopping after any given stage.
     */
    void setStopAfterStage_forTest(std::string stage);

private:
    // The _clonerName must be initialized before _mutex, as _clonerName is used to generate the
    // name of the _mutex.
    std::string _clonerName;

protected:
    enum AfterStageBehavior {
        // Continue to next stage.
        kContinueNormally,
        // Skip all remaining stages including postStage.
        kSkipRemainingStages,
    };

    // A ClonerStage is a retryable chunk of work usually based around a network operation.
    // The run() method does the work and, if an error occurs, throws an exception.  The
    // isTransientError() method determines whether the exception is retryable or not; usually
    // network errors will be retryable and other errors will not.  If the error is retryable,
    // the BaseCloner framework will attempt to reconnect the client and run the stage again.  If
    // it is not, the exception will be propagated up and fail the initial sync attempt entirely.
    class BaseClonerStage {
    public:
        BaseClonerStage(std::string name) : _name(name){};

        virtual AfterStageBehavior run() = 0;

        /**
         * Returns true if the Status represents an error which should be retried.
         */
        virtual bool isTransientError(const Status& status) {
            return ErrorCodes::isRetriableError(status);
        }

        /**
         * Returns true if the sync source validity should be checked before retrying.
         * This includes checking the sync source member state, checking the rollback ID,
         * and checking the sync source initial sync ID.
         * This method is provided because early stages which connect and collect
         * the initial sync ID must complete successfully before checking sync source validity.
         */
        virtual bool checkSyncSourceValidityOnRetry() {
            return true;
        }

        std::string getName() const {
            return _name;
        };

    private:
        std::string _name;
    };

    // The standard ClonerStage just refers back to a Cloner member function to do the work,
    // for syntactic convenience.
    template <class T>
    class ClonerStage : public BaseClonerStage {
    public:
        typedef AfterStageBehavior (T::*ClonerRunFn)(void);

        ClonerStage(std::string name, T* cloner, ClonerRunFn stageFunc)
            : BaseClonerStage(name), _cloner(cloner), _stageFunc(stageFunc) {}
        virtual AfterStageBehavior run() {
            return (_cloner->*_stageFunc)();
        }

    protected:
        T* getCloner() {
            return _cloner;
        }

    private:
        T* _cloner;
        ClonerRunFn _stageFunc;
    };

    typedef std::vector<BaseClonerStage*> ClonerStages;

    mutable Mutex _mutex = MONGO_MAKE_LATCH(_clonerName + "::_mutex"_sd);

    const StringData getClonerName() const {
        return _clonerName;
    }

    InitialSyncSharedData* getSharedData() const {
        return _sharedData;
    }

    DBClientConnection* getClient() const {
        return _client;
    }

    StorageInterface* getStorageInterface() const {
        return _storageInterface;
    }

    ThreadPool* getDBPool() const {
        return _dbPool;
    }

    bool isActive(WithLock) const {
        return _active;
    }

    Status getStatus(WithLock) const {
        return _status;
    }

    const HostAndPort& getSource() const {
        return _source;
    }

    /**
     * Examine the failpoint data and return true if it's for this cloner.  The base method
     * checks the "cloner" field against getClonerName() and should be called by overrides.
     */
    virtual bool isMyFailPoint(const BSONObj& data) const;

    /**
     * If initial sync status is OK, mark it failed.  Also set the local status.
     */
    void setInitialSyncFailedStatus(Status status);

    /**
     * Takes the initial sync status lock and checks the initial sync status.
     * Used to make sure failpoints exit on process shutdown.
     */
    bool mustExit();

    /**
     * A stage may, but is not required, to call this when we should clear the retrying state
     * because the operation has at least partially succeeded.  If the stage does not call this,
     * the retrying state is cleared upon successful completion of the entire stage.
     */
    void clearRetryingState();

private:
    virtual ClonerStages getStages() = 0;

    /**
     * Code to be run before and after the stages respectively.  This code is not subject to the
     * retry logic used in the cloner stages.
     */
    virtual void preStage() {}
    virtual void postStage() {}

    AfterStageBehavior runStage(BaseClonerStage* stage);

    AfterStageBehavior runStageWithRetries(BaseClonerStage* stage);

    /**
     * Make sure the initial sync ID on the sync source has not changed.  Throws an exception
     * if it has.  Returns a not-OK status if a network error occurs.
     */
    Status checkInitialSyncIdIsUnchanged();

    /**
     * Make sure the rollback ID has not changed.  Throws an exception if it has.  Returns
     * a not-OK status if a network error occurs.
     */
    Status checkRollBackIdIsUnchanged();

    /**
     * Does validity checks on the sync source.  If the sync source is now no longer usable,
     * throws an exception. Returns a not-OK status if a network error occurs or if the sync
     * source is temporarily unusable (e.g. restarting).
     */
    Status checkSyncSourceIsStillValid();

    /**
     * Supports pausing at certain stages for the initial sync fuzzer test framework.
     */
    void pauseForFuzzer(BaseClonerStage* stage);

    /**
     * Provides part of a log message for the initial sync describing the namespace the
     * cloner is operating on.  It must start with the database name, followed by the
     * string ' db: { ', followed by the stage name, followed by ': ' and the collection UUID
     * if known.
     */
    virtual std::string describeForFuzzer(BaseClonerStage*) const = 0;

    AfterStageBehavior runStages();

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to classes own rules
    // (M)  Reads and writes guarded by _mutex
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    InitialSyncSharedData* _sharedData;   // (S)
    DBClientConnection* _client;          // (X)
    StorageInterface* _storageInterface;  // (X)
    ThreadPool* _dbPool;                  // (X)
    HostAndPort _source;                  // (R)

    // _active indicates this cloner is being run, and is used only for status reporting and
    // invariant checking.
    bool _active = false;           // (M)
    Status _status = Status::OK();  // (M)
    // _startedAsync indicates the cloner is being run on some executor using runOnExecutorEvent(),
    // and is used only for invariant checking.
    bool _startedAsync = false;  // (M)
    // _promise corresponds to the Future returned by runOnExecutorEvent().  When not running
    // asynchronously, this is a null promise.
    Promise<void> _promise;  // (S)
    // _stopAfterStage is used for unit testing and causes the cloner to exit after a given
    // stage.
    std::string _stopAfterStage;  // (X)

    // Operation that may currently be retrying.
    InitialSyncSharedData::RetryableOperation _retryableOp;  // (X)
};

}  // namespace repl
}  // namespace mongo
