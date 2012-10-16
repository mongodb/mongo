// @file background.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string>
#include <vector>

#include "concurrency/spin_lock.h"

namespace mongo {

    /**
     *  Background thread dispatching.
     *  subclass and define run()
     *
     *  It is ok to call go(), that is, run the job, more than once -- if the
     *  previous invocation has finished. Thus one pattern of use is to embed
     *  a backgroundjob in your object and reuse it (or same thing with
     *  inheritance).  Each go() call spawns a new thread.
     *
     *  Thread safety:
     *    note when job destructs, the thread is not terminated if still running.
     *    generally if the thread could still be running, allocate the job dynamically
     *    and set deleteSelf to true.
     *
     *    go() and wait() are not thread safe
     *    run() will be executed on the background thread
     *    BackgroundJob object must exist for as long the background thread is running
     */

    class BackgroundJob : boost::noncopyable {
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
         *   if run() throws, the exception will be caught within 'this' object and will ultimately lead to the
         *   BackgroundJob's thread being finished, as if run() returned.
         *
         */
        virtual void run() = 0;

    public:
        enum State {
            NotStarted,
            Running,
            Done
        };

        virtual ~BackgroundJob() { }

        /**
         * starts job.
         * returns immediately after dispatching.
         *
         * @note the BackgroundJob object must live for as long the thread is still running, ie
         * until getState() returns Done.
         */
        BackgroundJob& go();

        /**
         * wait for completion.
         *
         * @param msTimeOut maximum amount of time to wait in milliseconds
         * @return true if did not time out. false otherwise.
         *
         * @note you can call wait() more than once if the first call times out.
         * but you cannot call wait on a self-deleting job.
         */
        bool wait( unsigned msTimeOut = 0 );

        // accessors
        State getState() const;
        bool running() const;

    private:
        struct JobStatus;
        boost::shared_ptr<JobStatus> _status;  // shared between 'this' and body() thread

        void jobBody( boost::shared_ptr<JobStatus> status );

    };
    
    /**
     * these run "roughly" every minute
     * instantiate statically
     * class MyTask : public PeriodicTask {
     * public:
     *   virtual std::string name() const { return "MyTask; " }
     *   virtual void doWork() { log() << "hi" << endl; }
     * } myTask;
     */
    class PeriodicTask {
    public:
        PeriodicTask();
        virtual ~PeriodicTask();

        virtual void taskDoWork() = 0;
        virtual std::string taskName() const = 0;

        class Runner : public BackgroundJob {
        public:
            virtual ~Runner(){}

            virtual std::string name() const { return "PeriodicTask::Runner"; }
            
            virtual void run();
            
            void add( PeriodicTask* task );
            void remove( PeriodicTask* task );

        private:
            
            SpinLock _lock;
            
            // these are NOT owned by Runner
            // Runner will not delete these
            // this never gets smaller
            // only fields replaced with nulls
            std::vector< PeriodicTask* > _tasks;

        };

        static Runner* theRunner;

    };




} // namespace mongo
