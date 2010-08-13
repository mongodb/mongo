// background.h

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

namespace mongo {

    /** object-orienty background thread dispatching.

       subclass and define run()

       It is ok to call go(), that is, run the job, more than once -- if the 
       previous invocation has finished. Thus one pattern of use is to embed 
       a backgroundjob in your object and reuse it (or same thing with 
       inheritance).  Each go() call spawns a new thread.

       note when job destructs, the thread is not terminated if still running.
       generally if the thread could still be running, allocate the job dynamically 
       and set deleteSelf to true.
    */
    /* example
    class ConnectBG : public BackgroundJob {
    public:
        int sock;
        int res;
        SockAddr farEnd;
        void run() {
            res = ::connect(sock, farEnd.raw(), farEnd.addressSize);
        }
    };
    */
    class BackgroundJob : boost::noncopyable {
    protected:
        /** define this to do your work.
            after this returns, state is set to done.
            after this returns, deleted if deleteSelf true.
        */
        virtual void run() = 0;
        virtual string name() = 0;
        //virtual void ending() { } // hook for post processing if desired after everything else done. not called when deleteSelf=true
    public:
        enum State {
            NotStarted,
            Running,
            Done
        };
        State getState() const { return state; }
        bool running() const   { return state == Running; }

        bool deleteSelf; // delete self when Done?

        bool nameThread; // thread should name itself to the OS / debugger. set to false if very short lived to avoid that call

        BackgroundJob() {
            deleteSelf = false;
            state = NotStarted;
            nameThread = true;
        }
        virtual ~BackgroundJob() { }

        // starts job.  returns once it is "dispatched"
        BackgroundJob& go();

        // wait for completion.  this spins with sleep() so not terribly efficient.
        // returns true if did not time out.
        //
        // note you can call wait() more than once if the first call times out.
        bool wait(int msMax = 0, unsigned maxSleepInterval=1000);

        /* start several */
        static void go(list<BackgroundJob*>&);

        /* wait for several jobs to finish. */
        static void wait(list<BackgroundJob*>&, unsigned maxSleepInterval=1000);

    private:
        //static BackgroundJob *grab;
        //static mongo::mutex mutex;
        void thr();
        volatile State state;
        boost::mutex _m;
        boost::condition _c;
    };

    class PeriodicBackgroundJob : public BackgroundJob {
    public:
        PeriodicBackgroundJob( int millisToSleep ) 
            : _millis( millisToSleep ){
        }
        
        virtual ~PeriodicBackgroundJob(){}

        /** this gets called every millisToSleep ms */
        virtual void runLoop() = 0;
        
        virtual void run();


    private:
        int _millis;
                
    };

} // namespace mongo
