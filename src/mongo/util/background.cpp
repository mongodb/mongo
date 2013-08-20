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

#include "mongo/pch.h"

#include "mongo/util/background.h"

#include <boost/thread/condition.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

using namespace std;
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
            massert( 13643, mongoutils::str::stream() << "backgroundjob already started: "
                                                      << name(),
                            status->state != Running );
            status->state = Running;
        }

        const string threadName = name();
        if( ! threadName.empty() )
            setThreadName( threadName.c_str() );

        try {
            run();
        }
        catch ( std::exception& e ) {
            error() << "backgroundjob " << name() << " exception: " << e.what() << endl;
        }
        catch(...) {
            error() << "uncaught exception in BackgroundJob " << name() << endl;
        }

        {
            scoped_lock l( status->m );
            status->state = Done;
            status->finished.notify_all();
        }

#ifdef MONGO_SSL
        SSLManagerInterface* manager = getSSLManager();
        if (manager)
            manager->cleanupThreadLocals();
#endif
        if( status->deleteSelf )
            delete this;
    }

    BackgroundJob& BackgroundJob::go() {
        boost::thread t( boost::bind( &BackgroundJob::jobBody , this, _status ) );
        return *this;
    }

    bool BackgroundJob::wait( unsigned msTimeOut ) {
        verify( !_status->deleteSelf ); // you cannot call wait on a self-deleting job
        scoped_lock l( _status->m );
        while ( _status->state != Done ) {
            if ( msTimeOut ) {
                // add msTimeOut millisecond to current time
                boost::xtime xt;
                boost::xtime_get( &xt, MONGO_BOOST_TIME_UTC );

                unsigned long long ns = msTimeOut * 1000000ULL; // milli to nano
                if ( xt.nsec + ns < 1000000000 ) {
                    xt.nsec = (boost::xtime::xtime_nsec_t) (xt.nsec + ns);
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

    // -------------------------

    PeriodicTask::PeriodicTask() {
        if ( ! theRunner )
            theRunner = new Runner();
        theRunner->add( this );
    }

    PeriodicTask::~PeriodicTask() {
        theRunner->remove( this );
    }

    void PeriodicTask::Runner::add( PeriodicTask* task ) {
        scoped_spinlock lk( _lock );
        _tasks.push_back( task );
    }
    
    void PeriodicTask::Runner::remove( PeriodicTask* task ) {
        scoped_spinlock lk( _lock );
        for ( size_t i=0; i<_tasks.size(); i++ ) {
            if ( _tasks[i] == task ) {
                _tasks[i] = 0;
                break;
            }
        }
    }

    void PeriodicTask::Runner::run() { 
        int sleeptime = 60;
        DEV sleeptime = 5; // to catch race conditions

        while ( ! inShutdown() ) {

            sleepsecs( sleeptime );
            
            scoped_spinlock lk( _lock );
            
            size_t size = _tasks.size();
            
            for ( size_t i=0; i<size; i++ ) {
                PeriodicTask * t = _tasks[i];
                if ( ! t )
                    continue;

                if ( inShutdown() )
                    break;
                
                Timer timer;
                try {
                    t->taskDoWork();
                }
                catch ( std::exception& e ) {
                    error() << "task: " << t->taskName() << " failed: " << e.what() << endl;
                }
                catch ( ... ) {
                    error() << "task: " << t->taskName() << " failed with unknown error" << endl;
                }
                
                int ms = timer.millis();
                LOG( ms <= 3 ? 3 : 0 ) << "task: " << t->taskName() << " took: " << ms << "ms" << endl;
            }
        }
    }

    PeriodicTask::Runner* PeriodicTask::theRunner = 0;

} // namespace mongo
