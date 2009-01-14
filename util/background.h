/**
*    Copyright (C) 2008 10gen Inc.
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
    static boost::mutex mutex;
    static void thr();
    volatile State state;
};

} // namespace mongo
