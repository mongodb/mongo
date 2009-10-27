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

    /* object-orienty background thread dispatching.

       subclass and define run()

       It is ok to call go() more than once -- if the previous invocation
       has finished. Thus one pattern of use is to embed a backgroundjob
       in your object and reuse it (or same thing with inheritance).
    */

    class BackgroundJob {
    protected:
        /* define this to do your work! */
        virtual void run() = 0;

    public:
        enum State {
            NotStarted,
            Running,
            Done
        };
        State getState() const {
            return state;
        }
        bool running() const {
            return state == Running;
        }

        bool deleteSelf; // delete self when Done?

        BackgroundJob() {
            deleteSelf = false;
            state = NotStarted;
        }
        virtual ~BackgroundJob() { }

        // start job.  returns before it's finished.
        BackgroundJob& go();

        // wait for completion.  this spins with sleep() so not terribly efficient.
        // returns true if did not time out.
        //
        // note you can call wait() more than once if the first call times out.
        bool wait(int msMax = 0);

    private:
        static BackgroundJob *grab;
        static boost::mutex &mutex;
        static void thr();
        volatile State state;
    };

} // namespace mongo
