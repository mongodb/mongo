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

#include "pch.h"
#include "goodies.h"
#include "background.h"
#include <list>

namespace mongo {

    BackgroundJob *BackgroundJob::grab = 0;
    mongo::mutex BackgroundJob::mutex("BackgroundJob");

    /* static */
    void BackgroundJob::thr() {
        assert( grab );
        BackgroundJob *us = grab;
        assert( us->state == NotStarted );
        us->state = Running;
        grab = 0;

        {
            string nm = us->name();
            setThreadName(nm.c_str());
        }

        try {
            us->run();
        }
        catch ( std::exception& e ){
            log( LL_ERROR ) << "backgroundjob error: " << e.what() << endl;
        }
        catch(...) {
            log( LL_ERROR ) << "uncaught exception in BackgroundJob" << endl;
        }
        us->state = Done;
        bool delSelf = us->deleteSelf;
        us->ending();
        if( delSelf ) 
            delete us;
    }

    BackgroundJob& BackgroundJob::go() {
        scoped_lock bl(mutex);
        assert( grab == 0 );
        grab = this;
        boost::thread t(thr);
        while ( grab )
            sleepmillis(2);
        return *this;
    }

    bool BackgroundJob::wait(int msMax, unsigned maxsleep) {
        assert( state != NotStarted );
        unsigned ms = 0;
        Date_t start = jsTime();
        while ( state != Done ) {
            sleepmillis(ms);
            if( ms*2<maxsleep ) ms*=2;
            if ( msMax && ( int( jsTime() - start ) > msMax) )
                return false;
        }
        return true;
    }

    void BackgroundJob::go(list<BackgroundJob*>& L) {
        for( list<BackgroundJob*>::iterator i = L.begin(); i != L.end(); i++ )
            (*i)->go();
    }

    /* wait for several jobs to finish. */
    void BackgroundJob::wait(list<BackgroundJob*>& L, unsigned maxsleep) {
        unsigned ms = 0;
        {
            x:
            sleepmillis(ms);
            if( ms*2<maxsleep ) ms*=2;
            for( list<BackgroundJob*>::iterator i = L.begin(); i != L.end(); i++ ) { 
                assert( (*i)->state != NotStarted );
                if( (*i)->state != Done )
                    goto x;
            }
        }
    }

} // namespace mongo
