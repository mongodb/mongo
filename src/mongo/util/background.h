// @file background.h

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"

namespace mongo {

/**
 *  Background thread dispatching.
 *  subclass and define run()
 *
 *  It is not possible to run the job more than once. An attempt to call 'go' while the
 *  task is running will fail. Calling 'go' after the task has finished are ignored and
 *  will not start the job again.
 *
 *  Thread safety: Note that when the job destructs, the thread is not terminated if still
 *  running. Generally, if the thread could still be running, allocate the job dynamically
 *  and set deleteSelf to true.
 *
 *  The overridden run() method will be executed on the background thread, so the
 *  BackgroundJob object must exist for as long the background thread is running.
 */

class BackgroundJob {
    MONGO_DISALLOW_COPYING(BackgroundJob);

protected:
    /**
     * sub-class must instantiate the BackgroundJob
     *
     * @param selfDelete if set to true, object will destruct itself after the run() finished
     * @note selfDelete instances cannot be wait()-ed upon
     */
    explicit BackgroundJob(bool selfDelete = false);

    virtual std::string name() const = 0;

    /**
     * define this to do your work.
     * after this returns, state is set to done.
     * after this returns, deleted if deleteSelf true.
     *
     * NOTE:
     *   if run() throws, the exception will be caught within 'this' object and will ultimately lead
     *   to the BackgroundJob's thread being finished, as if run() returned.
     *
     */
    virtual void run() = 0;

public:
    enum State { NotStarted, Running, Done };

    virtual ~BackgroundJob();

    /**
     * starts job.
     * returns immediately after dispatching.
     *
     * @note the BackgroundJob object must live for as long the thread is still running, ie
     * until getState() returns Done.
     */
    void go();


    /**
     * If the job has not yet started, transitions the job to the 'done' state immediately,
     * such that subsequent calls to 'go' are ignored, and notifies any waiters waiting in
     * 'wait'. If the job has already been started, this method returns a not-ok status: it
     * does not cancel running jobs. For this reason, you must still call 'wait' on a
     * BackgroundJob even after calling 'cancel'.
     */
    Status cancel();

    /**
     * wait for completion.
     *
     * @param msTimeOut maximum amount of time to wait in milliseconds
     * @return true if did not time out. false otherwise.
     *
     * @note you can call wait() more than once if the first call times out.
     * but you cannot call wait on a self-deleting job.
     */
    bool wait(unsigned msTimeOut = 0);

    // accessors. Note that while the access to the internal state is synchronized within
    // these methods, there is no guarantee that the BackgroundJob is still in the
    // indicated state after returning.
    State getState() const;
    bool running() const;

private:
    const bool _selfDelete;

    struct JobStatus;
    const std::unique_ptr<JobStatus> _status;

    void jobBody();
};

/**
 * these run "roughly" every minute
 * instantiate statically
 * class MyTask : public PeriodicTask {
 * public:
 *   virtual std::string name() const { return "MyTask; " }
 *   virtual void doWork() { log() << "hi" << std::endl; }
 * } myTask;
 */
class PeriodicTask {
public:
    PeriodicTask();
    virtual ~PeriodicTask();

    virtual void taskDoWork() = 0;
    virtual std::string taskName() const = 0;

    /**
     *  Starts the BackgroundJob that runs PeriodicTasks. You may call this multiple times,
     *  from multiple threads, and the BackgroundJob will be started only once. Please note
     *  that since this method starts threads, it is not appropriate to call it from within
     *  a mongo initializer. Calling this method after calling 'stopRunningPeriodicTasks'
     *  does not re-start the background job.
     */
    static void startRunningPeriodicTasks();

    /**
     *  Waits 'gracePeriodMillis' for the BackgroundJob responsible for PeriodicTask
     *  execution to finish any running tasks, then destroys it. If the BackgroundJob was
     *  never started, returns Status::OK right away. If the BackgroundJob does not
     *  terminate within the grace period, returns an invalid status. It is safe to call
     *  this method repeatedly from one thread if the grace period is overshot. It is not
     *  safe to call this method from multiple threads, or in a way that races with
     *  'startRunningPeriodicTasks'.
     */
    static Status stopRunningPeriodicTasks(int gracePeriodMillis);
};


}  // namespace mongo
