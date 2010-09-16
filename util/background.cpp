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

#include <list>

#include "goodies.h"
#include "time_support.h"
#include "background.h"

namespace mongo {

// BackgroundJob *BackgroundJob::grab = 0;
//    mongo::mutex BackgroundJob::mutex("BackgroundJob");

    void BackgroundJob::thr() {
        assert( state == NotStarted );
        state = Running;

        if( nameThread ) {
            string nm = name();
            setThreadName(nm.c_str());
        }

        try {
            run();
        }
        catch ( std::exception& e ){
            log( LL_ERROR ) << "backgroundjob " << name() << "error: " << e.what() << endl;
        }
        catch(...) {
            log( LL_ERROR ) << "uncaught exception in BackgroundJob " << name() << endl;
        }
        state = Done;
        bool delSelf = deleteSelf;
        //ending();
        if( delSelf ) 
            delete this;
    }

    BackgroundJob& BackgroundJob::go() {
        boost::thread t( boost::bind(&BackgroundJob::thr, this) );
        return *this;
    }

    bool BackgroundJob::wait(int msMax, unsigned maxsleep) {
        unsigned ms = 1;
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
        unsigned ms = 1;
        {
            x:
            sleepmillis(ms);
            if( ms*2<maxsleep ) ms*=2;
            for( list<BackgroundJob*>::iterator i = L.begin(); i != L.end(); i++ ) { 
                //assert( (*i)->state != NotStarted );
                if( (*i)->state != Done )
                    goto x;
            }
        }
    }
    
} // namespace mongo
