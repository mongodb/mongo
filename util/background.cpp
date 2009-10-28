//background.cpp

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

#include "stdafx.h"
#include "goodies.h"
#include "background.h"

namespace mongo {

    BackgroundJob *BackgroundJob::grab = 0;
    boost::mutex &BackgroundJob::mutex = *( new boost::mutex );

    /* static */
    void BackgroundJob::thr() {
        assert( grab );
        BackgroundJob *us = grab;
        assert( us->state == NotStarted );
        us->state = Running;
        grab = 0;
        us->run();
        us->state = Done;
        if ( us->deleteSelf )
            delete us;
    }

    BackgroundJob& BackgroundJob::go() {
        boostlock bl(mutex);
        assert( grab == 0 );
        grab = this;
        boost::thread t(thr);
        while ( grab )
            sleepmillis(2);
        return *this;
    }

    bool BackgroundJob::wait(int msMax) {
        assert( state != NotStarted );
        int ms = 1;
        unsigned long long start = jsTime();
        while ( state != Done ) {
            sleepmillis(ms);
            if ( ms < 1000 )
                ms = ms * 2;
            if ( msMax && ( int( jsTime() - start ) > msMax) )
                return false;
        }
        return true;
    }

} // namespace mongo
