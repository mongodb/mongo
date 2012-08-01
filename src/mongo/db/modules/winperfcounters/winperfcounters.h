/** @file mongo/db/modules/winperfcounters/winperfcounters.h - Windows performance counters for mongod.exe */

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

#pragma once

#if defined (_WIN32)

#include "mongo/db/module.h"
#include "mongo/util/background.h"

namespace mongo {

    /**
     * Windows performance counters for mongod.exe, implemented as a Module.
     *
     * This module adds a command line option: "--winPerfCounters".
     *
     * If the option is not defined at Module::config() time, this module does nothing,
     * costs nothing.
     * If defined, a new instance of the counters defined in the XML file "mongod.man" is made
     * available on the OS.
     * The counters are updated only when there are active Windows performance counter clients
     * requesting these counters. The counters will then be updated about once every second,
     * with very little/negligible CPU cost.
     * When there are no active clients this module should have zero CPU cost.
     *
     * There can exist only one instance of this class at any time.
     *
     * Do read the README for important information on how to use, how to create new counters,
     * implementation details and caveats.
     */
    class WinPerfCounters : public Module, BackgroundJob {
    public:
        WinPerfCounters();
        virtual ~WinPerfCounters();

        // Module interface
        virtual void config( boost::program_options::variables_map& params );
        virtual void init();
        virtual void shutdown();

    protected:
        // BackgroundJob interface
        virtual std::string name() const;
        virtual void run();

        // not private for testability
        PPERF_COUNTERSET_INSTANCE _setInstance;

    private:
        // LOG() level for start/stop updating counters
        static  int const LL_STARTSTOP = 1;

        // LOG() level for controlCallback() ADD/REMOVE events
        static  int const LL_CALLBACK = 6;

        // LOG() level for updateCounters()
        static  int const LL_UPDATE = 7;

        // how often to updateCounters()
        // per Windows SDK, clients must wait a minimum of 1 sec between samples
        static unsigned int const UPDATE_INTERVAL_MS = 950;

        // shutdown() is expected to complete in a few ms, this is a safeguard
        static unsigned int const SHUTDOWN_TIMEOUT_MS = 10*1000;

        // the only instance of this class
        static WinPerfCounters* volatile _this;

        bool volatile       _isInited;                  // ready to go() or already running()
        bool                _configEnableModule;        // --winPerfCounters
        int volatile        _clientsCnt;                // number of active perf counter clients
        HANDLE              _hTimer;                    // waitable timer, updateCounters() heartbeat
        HANDLE              _hEndRun;                   // event, signals run() to end
        std::wstring        _perf_instance_name;        // the instance name that clients see

        // protects controlCallback()
        SimpleMutex _mutexCallback;

        void startTimer();

        // PERFLIBREQUEST type function, called by thread injected by Windows
        static ULONG WINAPI controlCallback( ULONG requestCode, PVOID buffer, ULONG bufferSize );

        void setULONG( ULONG counter_id, ULONG value );
        void setULONGLONG( ULONG counter_id, ULONGLONG value );

        // virtual for testability
        virtual std::wstring perf_instance_name() const;
        virtual void updateCounters();
    };

} // namespace mongo

#endif
