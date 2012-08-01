/** @file mongo/db/modules/winperfcounters/winperfcounters.cpp - Windows performance counters for mongod.exe */

/*
 *    Copyright (C) 2012 Erich Siedler <erich.siedler@gmail.com>
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

#if defined (_WIN32)

#include "mongo/pch.h"

#include "mongo/db/modules/winperfcounters/winperfcounters.h"
#include "winperfcounters_ctrpp.h" // generated from mongod.man by Windows SDK's ctrpp.exe during build

#include <boost/lexical_cast.hpp>

#include "mongo/db/client.h"
#include "mongo/db/d_globals.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/ntservice.h"

namespace mongo {

    WinPerfCounters::WinPerfCounters()
        : Module( "Windows Performance Counters" )
        , _isInited( false )
        , _configEnableModule( false )
        , _clientsCnt( 0 )
        , _hTimer( NULL )
        , _hEndRun( NULL )
        , _setInstance( NULL )
        , _mutexCallback( "WinPerfCounters_Callback" ) {

        verify( ! _this );
        _this = this;
        add_options()( "winPerfCounters", "enable windows performance counters" );
    }

    WinPerfCounters::~WinPerfCounters() {
        shutdown();
        _this = 0;
    }

    void WinPerfCounters::config( boost::program_options::variables_map& params ) {
        if ( params.count( "winPerfCounters" ) ) {
            _configEnableModule = true;
        }
    }

    // if init() fails we just disable this module and leave the system in a clean state
    void WinPerfCounters::init() {
        verify( ! _isInited );

        if ( ! _configEnableModule ) {
            LOG( 1 ) << "WinPerfCounters not enabled" << endl;
            return;
        }

        if ( NULL == ( _hEndRun = ::CreateEvent( /*security=*/ NULL, /*manual=*/ TRUE, /*state=*/ FALSE, /*name=*/ NULL ) ) ) {
            error() << "WinPerfCounters::init() failed on CreateEvent(): " << ::GetLastError() << endl;
            return;
        }

        if ( NULL == ( _hTimer = ::CreateWaitableTimer( /*security=*/ NULL, /*manual=*/ FALSE, /*name=*/ NULL ) ) ) {
            error() << "WinPerfCounters::init() failed on CreateWaitableTimer(): " << ::GetLastError() << endl;
            wassert( ::CloseHandle( _hEndRun ) );
            _hEndRun = NULL;
            return;
        }

        {
            // controlCallback() might get called right after Ctrpp_CounterInitialize(), lock will make it
            // wait until we have finished initializing
            SimpleMutex::scoped_lock lock( _this->_mutexCallback );

            // Ctrpp_CounterInitialize() created by ctrpp.exe
            PERF_MEM_ALLOC const no_custom_alloc = NULL; PERF_MEM_FREE const no_custom_free = NULL; PVOID const no_custom_arg = NULL;
            ULONG ci_res = ! ERROR_SUCCESS;
            if ( ERROR_SUCCESS != ( ci_res = Ctrpp_CounterInitialize( controlCallback, no_custom_alloc, no_custom_free, no_custom_arg ) ) ) {
                error() << "WinPerfCounters::init() failed on Ctrpp_CounterInitialize(): " << ci_res << endl;
                wassert( ::CloseHandle( _hEndRun ) );
                _hEndRun = NULL;
                wassert( ::CloseHandle( _hTimer ) );
                _hTimer = NULL;
                return;
            }

            // make the instance name the same as the service name
            _perf_instance_name = ServiceController::getServiceName();
            if ( _perf_instance_name.empty() ) {
                // not running as a service; make a name unique to multiple instances, unlike to collide with
                // mongodb services, and easy for the user to relate the name to the database
                _perf_instance_name = L"Listening_" + std::wstring( boost::lexical_cast< std::wstring >( cmdLine.port ) );
            }

            // perf_instance_name + iindex uniquely identify an instance systemwide (the SDK is lacking and confusing about iindex)
            ULONG const iindex = 0;
            if ( NULL == ( _setInstance = ::PerfCreateInstance( Ctrpp_MongoDbProvider, &Ctrpp_MongoDbGuid, perf_instance_name().c_str(), iindex ) ) ) {
                error() << "WinPerfCounters::init() failed on PerfCreateInstance(): " << ::GetLastError() << endl;
                wassert( ::CloseHandle( _hEndRun ) );
                _hEndRun = NULL;
                wassert( ::CloseHandle( _hTimer ) );
                _hTimer = NULL;
                Ctrpp_CounterCleanup();
                return;
            }

            _isInited = true;
        }

        // start BackgroundJob
        go();
    }

    void WinPerfCounters::shutdown() {
        if ( ! _isInited )
            return;

        wassert( ::SetEvent( _hEndRun ) );
        wassert( wait( SHUTDOWN_TIMEOUT_MS ) );
        wassert( !running() );

        wassert( ERROR_SUCCESS == ::PerfDeleteInstance( Ctrpp_MongoDbProvider, _setInstance ) );
        _setInstance = NULL;
        Ctrpp_CounterCleanup();
        wassert( ::CloseHandle( _hEndRun ) );
        _hEndRun = NULL;
        wassert( ::CloseHandle( _hTimer ) );
        _hTimer = NULL;

        _isInited = false;
    }

    // BackgroundJob thread name
    std::string WinPerfCounters::name() const {
        return "winPerfCounters";
    }

    // BackgroundJob main
    void WinPerfCounters::run() {
        verify( _isInited );

        log() << "WinPerfCounters starting: instance: " << toUtf8String( perf_instance_name() )
              << " update interval: " << UPDATE_INTERVAL_MS << " ms" << endl;

        Client::initThread( name().c_str() );
        updateCounters();

        HANDLE handles[] = { _hEndRun, _hTimer };
        DWORD const IDX_HND_END = WAIT_OBJECT_0; DWORD const IDX_HND_TIMER = WAIT_OBJECT_0 + 1;
        DWORD wait = WAIT_FAILED;

        while ( IDX_HND_TIMER == ( wait = ::WaitForMultipleObjects( 2, handles, /*wait all=*/ FALSE, INFINITE ) ) ) {
            LOG( LL_UPDATE ) << "WinPerfCounters updating counters" << endl;
            updateCounters();
        }
        wassert( IDX_HND_END == wait );

        cc().shutdown();
    }

    std::wstring WinPerfCounters::perf_instance_name() const {
        return _perf_instance_name;
    }

    void WinPerfCounters::startTimer() {
        verify( NULL != _hTimer );

        LARGE_INTEGER const     signal_now = { 0 };
        PTIMERAPCROUTINE const  no_completion_func = NULL;
        LPVOID const            no_arg = NULL;
        BOOL const              resume_FALSE = FALSE; // don't resume a suspended power system
        wassert( ::SetWaitableTimer( _hTimer, &signal_now, UPDATE_INTERVAL_MS, no_completion_func, no_arg, resume_FALSE ) );
    }

    // Will be called from a thread (or more?) injected by Windows, must return in less than a second.
    // Must be VERY careful with what is called. For example: no updateCounters(), no cc().
    //
    //
    // The SDK states that we can use PERF_ADD_COUNTER and PERF_REMOVE_COUNTER to start and stop updating
    // the counters, but, at least in Vista SP2, perflib2 is very buggy in that respect. I've hit many
    // scenarios in which a single or many PERF_ADD_COUNTER or PERF_REMOVE_COUNTER are missing.
    //
    // The Vista SP2/Server 2008 SP2 hotfix KB970838 fixes only one of such buggy behaviour.
    //
    // This implementation accounts for all observed bugs, no windows performance counter client will ever see
    // a problem. There is one small downside: IF we already have/had one active client, AND we hit one of this
    // windows bugs, then we will never stop updating the counters ( i.e., will behave like there is always one
    // active client ).
    ULONG WINAPI WinPerfCounters::controlCallback( ULONG requestCode, PVOID /*buffer*/, ULONG /*bufferSize*/ ) {
        verify( _this );
        verify( NULL != _this->_hTimer );

        SimpleMutex::scoped_lock lock( _this->_mutexCallback );

        switch ( requestCode ) {
        case PERF_ADD_COUNTER:
            LOG( LL_CALLBACK ) << "WinPerfCounters received PERF_ADD_COUNTER"  << endl;
            if ( 1 == ++_this->_clientsCnt ) {
                LOG( LL_STARTSTOP ) << "WinPerfCounters have clients, started updating counters" << endl;
                _this->startTimer();
            };
            break;

        case PERF_REMOVE_COUNTER:
            LOG( LL_CALLBACK ) << "WinPerfCounters received PERF_REMOVE_COUNTER" << endl;
            {
                int sc = --_this->_clientsCnt;
                if ( 0 == sc ) {
                    LOG( LL_STARTSTOP ) << "WinPerfCounters have no more clients, stopped updating counters" << endl;
                    wassert( ::CancelWaitableTimer( _this->_hTimer ) );
                }
                else if ( sc < 0 ) {
                    // received more REMOVEs than ADDs
                    warning() << "WinPerfCounters client tracking imbalance, collecting data permanently (harmless)" << endl;
                    _this->_clientsCnt = 1000 * 1000 * 1000;
                }
            }
            break;

        case PERF_COLLECT_START:
            if ( _this->_clientsCnt <= 0 ) {
                warning() << "WinPerfCounters unexpected start, collecting data permanently (harmless)" << endl;
                _this->_clientsCnt = 1000 * 1000 * 1000;
                _this->startTimer();
            }
            break;
        }
        wassert( _this->_clientsCnt >= 0 );
        return ERROR_SUCCESS;
    }

    inline void WinPerfCounters::setULONG( ULONG counter_id, ULONG value ) {
        wassert( ERROR_SUCCESS == ::PerfSetULongCounterValue( Ctrpp_MongoDbProvider, _setInstance, ( counter_id ), ( value ) ) );
    }

    inline void WinPerfCounters::setULONGLONG( ULONG counter_id, ULONGLONG value ) {
        wassert( ERROR_SUCCESS == ::PerfSetULongLongCounterValue( Ctrpp_MongoDbProvider, _setInstance, ( counter_id ), ( value ) ) );
    }

    // To add new counters: read README, be light, don't throw, update tests
    //
    // Update the counters with the API ::PerfSetULong[Long]CounterValue(), ::PerfIncrementULong[Long]CounterValue(),
    // ::PerfDecrementULong[Long]CounterValue().
    // Although the SDK says you can bypass these and access the raw counter in _setInstance, I currently recommend against it.
    void WinPerfCounters::updateCounters() {
        verify( _isInited );
        verify( NULL != _setInstance );

        {
            setULONG( MongoDb_Connections, connTicketHolder.used() );
            setULONG( MongoDb_ConnectionsAvailable, connTicketHolder.available() );
        }

        {
            ULONGLONG in = networkCounter.getBytesIn(), out = networkCounter.getBytesOut(), req = networkCounter.getRequests();
            setULONGLONG( MongoDb_NetworkBytesIn, in );
            setULONGLONG( MongoDb_NetworkBytesInSec, in );
            setULONGLONG( MongoDb_NetworkBytesOut, out );
            setULONGLONG( MongoDb_NetworkBytesOutSec, out );
            setULONGLONG( MongoDb_NetworkRequests, req );
            setULONGLONG( MongoDb_NetworkRequestsSec, req );
        }

        {
            int w=0, r=0;
            Client::recommendedYieldMicros( &w , &r );
            setULONG( MongoDb_gL_CurrentQueueTotal, w + r );
            setULONG( MongoDb_gL_CurrentQueueReaders, r );
            setULONG( MongoDb_gL_CurrentQueueWriters, w );
        }

        {
            int w=0, r=0;
            Client::getActiveClientCount( w , r );
            setULONG( MongoDb_gL_ClientsTotal, w + r );
            setULONG( MongoDb_gL_ClientsReaders, r );
            setULONG( MongoDb_gL_ClientsWriters, w );
        }

        {
            ULONG       regular = assertionCount.regular;
            ULONG       warning = assertionCount.warning;
            ULONG       msg     = assertionCount.msg;
            ULONG       user    = assertionCount.user;
            ULONGLONG   total   = regular + warning + msg + user;
            setULONGLONG( MongoDb_AssertsTotal,         total );
            setULONGLONG( MongoDb_AssertsTotalPerSec,   total );
            setULONG( MongoDb_AssertsRollovers,     assertionCount.rollovers );
            setULONG( MongoDb_AssertsRegular,       regular );
            setULONG( MongoDb_AssertsWarning,       warning );
            setULONG( MongoDb_AssertsMsg,           msg );
            setULONG( MongoDb_AssertsUser,          user );
        }

        {
            unsigned insert     = globalOpCounters.getInsert()->get();
            unsigned query      = globalOpCounters.getQuery()->get();
            unsigned update     = globalOpCounters.getUpdate()->get();
            unsigned del        = globalOpCounters.getDelete()->get();
            unsigned getmore    = globalOpCounters.getGetMore()->get();
            unsigned command    = globalOpCounters.getCommand()->get();
            ULONGLONG total     = insert + query + update + del + getmore + command;
            setULONGLONG( MongoDb_OpPerSec, total );
            setULONG( MongoDb_OpInsertPerSec,   insert );
            setULONG( MongoDb_OpQueryPerSec,    query );
            setULONG( MongoDb_OpUpdatePerSec,   update );
            setULONG( MongoDb_OpDeletePerSec,   del );
            setULONG( MongoDb_OpGetMorePerSec,  getmore );
            setULONG( MongoDb_OpCommandPerSec,  command );
        }
    }



    WinPerfCounters* volatile WinPerfCounters::_this = 0;

}  // namespace mongo

#endif
