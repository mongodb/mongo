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

#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/thread/condition.hpp>
#include <boost/thread/once.hpp>
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

    namespace {

        class PeriodicTaskRunner : public BackgroundJob {
        public:

            PeriodicTaskRunner()
                : _mutex("PeriodicTaskRunner")
                , _shutdownRequested(false) {}

            void add( PeriodicTask* task );
            void remove( PeriodicTask* task );

            Status stop( int gracePeriodMillis );

        private:

            virtual std::string name() const {
                return "PeriodicTaskRunner";
            }

            virtual void run();

            // Returns true if shutdown has been requested.  You must hold _mutex to call this
            // function.
            bool _isShutdownRequested() const;

            // Runs all registered tasks. You must hold _mutex to call this function.
            void _runTasks();

            // Runs one task to completion, and optionally reports timing. You must hold _mutex
            // to call this function.
            void _runTask( PeriodicTask* task );

            // _mutex protects the _shutdownRequested flag and the _tasks vector.
            mongo::mutex _mutex;

            // The condition variable is used to sleep for the interval between task
            // executions, and is notified when the _shutdownRequested flag is toggled.
            boost::condition _cond;

            // Used to break the loop. You should notify _cond after changing this to true
            // so that shutdown proceeds promptly.
            bool _shutdownRequested;

            // The PeriodicTasks contained in this vector are NOT owned by the
            // PeriodicTaskRunner, and are not deleted. The vector never shrinks, removed Tasks
            // have their entry overwritten with NULL.
            std::vector< PeriodicTask* > _tasks;
        };

        // We rely here on zero-initialization of 'runnerMutex' to distinguish whether we are
        // running before or after static initialization for this translation unit has
        // completed. In the former case, we assume no threads are present, so we do not need
        // to use the mutex. When present, the mutex protects 'runner' and 'runnerDestroyed'
        // below.
        SimpleMutex* const runnerMutex = new SimpleMutex("PeriodicTaskRunner");

        // A scoped lock like object that only locks/unlocks the mutex if it exists.
        class ConditionalScopedLock {
        public:
            ConditionalScopedLock( SimpleMutex* mutex ) : _mutex( mutex ) {
                if ( _mutex )
                    _mutex->lock();
            }
            ~ConditionalScopedLock() {
                if ( _mutex )
                    _mutex->unlock();
            }
        private:
            SimpleMutex* const _mutex;
        };

        // The unique PeriodicTaskRunner, also zero-initialized.
        PeriodicTaskRunner* runner;

        // The runner is never re-created once it has been destroyed.
        bool runnerDestroyed;

    } // namespace

    // both the BackgroundJob and the internal thread point to JobStatus
    struct BackgroundJob::JobStatus {
        JobStatus()
            : mutex( "backgroundJob" )
            , state( NotStarted ) {
        }

        mongo::mutex mutex;
        boost::condition done;
        State state;
    };

    BackgroundJob::BackgroundJob( bool selfDelete )
        : _selfDelete( selfDelete )
        , _status( new JobStatus ) {
    }

    BackgroundJob::~BackgroundJob() {}

    void BackgroundJob::jobBody() {

        const string threadName = name();
        if( ! threadName.empty() )
            setThreadName( threadName.c_str() );

        LOG(1) << "BackgroundJob starting: " << threadName << endl;

        try {
            run();
        }
        catch ( std::exception& e ) {
            error() << "backgroundjob " << threadName << " exception: " << e.what() << endl;
        }
        catch(...) {
            error() << "uncaught exception in BackgroundJob " << threadName << endl;
        }

        // We must cache this value so that we can use it after we leave the following scope.
        const bool selfDelete = _selfDelete;

        {
            // It is illegal to access any state owned by this BackgroundJob after leaving this
            // scope, with the exception of the call to 'delete this' below.
            scoped_lock l( _status->mutex );
            _status->state = Done;
            _status->done.notify_all();
        }

#ifdef MONGO_SSL
        // TODO(sverch): Allow people who use the BackgroundJob to also specify cleanup tasks.
        // Currently the networking code depends on this class and this class depends on the
        // networking code because of this ad hoc cleanup.
        SSLManagerInterface* manager = getSSLManager();
        if (manager)
            manager->cleanupThreadLocals();
#endif

        if( selfDelete )
            delete this;
    }

    void BackgroundJob::go() {
        scoped_lock l( _status->mutex );
        massert( 17234, mongoutils::str::stream()
                 << "backgroundJob already running: " << name(),
                 _status->state != Running );

        // If the job is already 'done', for instance because it was cancelled or already
        // finished, ignore additional requests to run the job.
        if (_status->state == NotStarted) {
            boost::thread t( boost::bind( &BackgroundJob::jobBody , this ) );
            _status->state = Running;
        }
    }

    Status BackgroundJob::cancel() {
        scoped_lock l( _status->mutex );

        if ( _status->state == Running )
            return Status( ErrorCodes::IllegalOperation,
                           "Cannot cancel a running BackgroundJob" );

        if ( _status->state == NotStarted ) {
            _status->state = Done;
            _status->done.notify_all();
        }

        return Status::OK();
    }

    bool BackgroundJob::wait( unsigned msTimeOut ) {
        verify( !_selfDelete ); // you cannot call wait on a self-deleting job
        scoped_lock l( _status->mutex );
        while ( _status->state != Done ) {
            if ( msTimeOut ) {
                boost::xtime deadline = incxtimemillis( msTimeOut );
                if ( !_status->done.timed_wait( l.boost() , deadline ) )
                    return false;
            }
            else {
                _status->done.wait( l.boost() );
            }
        }
        return true;
    }

    BackgroundJob::State BackgroundJob::getState() const {
        scoped_lock l( _status->mutex );
        return _status->state;
    }

    bool BackgroundJob::running() const {
        scoped_lock l( _status->mutex );
        return _status->state == Running;
    }

    // -------------------------

    PeriodicTask::PeriodicTask() {
        ConditionalScopedLock lock( runnerMutex );
        if ( runnerDestroyed )
            return;

        if ( !runner )
            runner = new PeriodicTaskRunner;

        runner->add( this );
    }

    PeriodicTask::~PeriodicTask() {
        ConditionalScopedLock lock( runnerMutex );
        if ( runnerDestroyed || !runner )
            return;

        runner->remove( this );
    }

    void PeriodicTask::startRunningPeriodicTasks() {
        ConditionalScopedLock lock( runnerMutex );
        if ( runnerDestroyed )
            return;

        if ( !runner )
            runner = new PeriodicTaskRunner;

        runner->go();
    }

    Status PeriodicTask::stopRunningPeriodicTasks( int gracePeriodMillis ) {
        ConditionalScopedLock lock( runnerMutex );

        Status status = Status::OK();
        if ( runnerDestroyed || !runner )
            return status;

        runner->cancel();
        status = runner->stop( gracePeriodMillis );

        if ( status.isOK() ) {
            delete runner;
            runnerDestroyed = true;
        }

        return status;
    }

    void PeriodicTaskRunner::add( PeriodicTask* task ) {
        mutex::scoped_lock lock( _mutex );
        _tasks.push_back( task );
    }

    void PeriodicTaskRunner::remove( PeriodicTask* task ) {
        mutex::scoped_lock lock( _mutex );
        for ( size_t i = 0; i != _tasks.size(); i++ ) {
            if ( _tasks[i] == task ) {
                _tasks[i] = NULL;
                break;
            }
        }
    }

    Status PeriodicTaskRunner::stop( int gracePeriodMillis ) {
        {
            mutex::scoped_lock lock( _mutex );
            _shutdownRequested = true;
            _cond.notify_one();
        }

        if ( !wait( gracePeriodMillis ) ) {
            return Status( ErrorCodes::ExceededTimeLimit,
                           "Grace period expired while waiting for PeriodicTasks to terminate" );
        }
        return Status::OK();
    }

    void PeriodicTaskRunner::run() {
        // Use a shorter cycle time in debug mode to help catch race conditions.
        const size_t waitMillis = (debug ? 5 : 60) * 1000;

        const boost::function<bool()> predicate =
            boost::bind( &PeriodicTaskRunner::_isShutdownRequested, this );

        mutex::scoped_lock lock( _mutex );
        while ( !predicate() ) {
            const boost::xtime deadline = incxtimemillis( waitMillis );
            if ( !_cond.timed_wait( lock.boost(), deadline, predicate ) )
                _runTasks();
        }
    }

    bool PeriodicTaskRunner::_isShutdownRequested() const {
        return _shutdownRequested;
    }

    void PeriodicTaskRunner::_runTasks() {
        const size_t size = _tasks.size();
        for ( size_t i = 0; i != size; ++i )
            if ( PeriodicTask* const task = _tasks[i] )
                _runTask(task);
    }

    void PeriodicTaskRunner::_runTask(PeriodicTask* const task) {
        Timer timer;

        const std::string taskName = task->taskName();

        try {
            task->taskDoWork();
        }
        catch ( const std::exception& e ) {
            error() << "task: " << taskName << " failed: " << e.what() << endl;
        }
        catch ( ... ) {
            error() << "task: " << taskName << " failed with unknown error" << endl;
        }

        const int ms = timer.millis();
        LOG( ms <= 3 ? 3 : 0 ) << "task: " << taskName << " took: " << ms << "ms" << endl;
    }

} // namespace mongo
