// @file background.cpp

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

#include "concurrency/mutex.h"

#include "background.h"

#include "mongoutils/str.h"

namespace mongo {

    // both the BackgroundJob and the internal thread point to JobStatus
    struct BackgroundJob::JobStatus {
        JobStatus( bool delFlag )
            : deleteSelf(delFlag), m("backgroundJob"), state(NotStarted) { }

        const bool deleteSelf;

        mongo::mutex m;  // protects state below
        boost::condition finished; // means _state == Done
        State state;
    };

    BackgroundJob::BackgroundJob( bool selfDelete ) {
        _status.reset( new JobStatus( selfDelete ) );
    }

    // Background object can be only be destroyed after jobBody() ran
    void BackgroundJob::jobBody( boost::shared_ptr<JobStatus> status ) {
        LOG(1) << "BackgroundJob starting: " << name() << endl;
        {
            scoped_lock l( status->m );
            massert( 13643 , mongoutils::str::stream() << "backgroundjob already started: " << name() , status->state == NotStarted );
            status->state = Running;
        }

        const string threadName = name();
        if( ! threadName.empty() )
            setThreadName( threadName.c_str() );

        try {
            run();
        }
        catch ( std::exception& e ) {
            log( LL_ERROR ) << "backgroundjob " << name() << "error: " << e.what() << endl;
        }
        catch(...) {
            log( LL_ERROR ) << "uncaught exception in BackgroundJob " << name() << endl;
        }

        {
            scoped_lock l( status->m );
            status->state = Done;
            status->finished.notify_all();
        }

        if( status->deleteSelf )
            delete this;
    }

    BackgroundJob& BackgroundJob::go() {
        boost::thread t( boost::bind( &BackgroundJob::jobBody , this, _status ) );
        return *this;
    }

    bool BackgroundJob::wait( unsigned msTimeOut ) {
        scoped_lock l( _status->m );
        while ( _status->state != Done ) {
            if ( msTimeOut ) {
                // add msTimeOut millisecond to current time
                boost::xtime xt;
                boost::xtime_get( &xt, boost::TIME_UTC );

                unsigned long long ns = msTimeOut * 1000000ULL; // milli to nano
                if ( xt.nsec + ns < 1000000000 ) {
                    xt.nsec = (xtime::xtime_nsec_t) (xt.nsec + ns);
                }
                else {
                    xt.sec += 1 + ns / 1000000000;
                    xt.nsec = ( ns + xt.nsec ) % 1000000000;
                }

                if ( ! _status->finished.timed_wait( l.boost() , xt ) )
                    return false;

            }
            else {
                _status->finished.wait( l.boost() );
            }
        }
        return true;
    }

    BackgroundJob::State BackgroundJob::getState() const {
        scoped_lock l( _status->m);
        return _status->state;
    }

    bool BackgroundJob::running() const {
        scoped_lock l( _status->m);
        return _status->state == Running;
    }

} // namespace mongo
