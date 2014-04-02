// signal_handlers.cpp

/**
*    Copyright (C) 2010 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/util/signal_handlers.h"

#include <boost/thread.hpp>

#include "mongo/db/client.h"
#include "mongo/db/log_process_details.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"

#if defined(_WIN32)
#  include "mongo/util/signal_win32.h"
#  include "mongo/util/exception_filter_win32.h"
#else
#  include <signal.h>
#  include <unistd.h>
#endif

#if defined(_WIN32)
namespace {
    const char* strsignal(int signalNum) {
        // should only see SIGABRT on windows
        switch (signalNum) {
        case SIGABRT: return "SIGABRT";
        default: return "UNKNOWN";
        }
    }
}
#endif

namespace mongo {

    /*
     * WARNING: PLEASE READ BEFORE CHANGING THIS MODULE
     *
     * All code in this module must be signal-friendly. Before adding any system
     * call or other dependency, please make sure that this still holds.
     *
     * All code in this file follows this pattern:
     *   Generic code
     *   #ifdef _WIN32
     *       Windows code
     *   #else
     *       Posix code
     *   #endif
     *
     */

    // everything provides this, but only header is instance.h
    void exitCleanly(ExitCode exitCode);

namespace {

    // This should only be used with MallocFreeOSteam
    class MallocFreeStreambuf : public std::streambuf {
        MONGO_DISALLOW_COPYING(MallocFreeStreambuf);
    public:
        MallocFreeStreambuf() {
            setp(_buffer, _buffer + maxLogLineSize);
        }

        StringData str() const { return StringData(pbase(), pptr() - pbase()); }
        void rewind() { setp(pbase(), epptr()); }

    private:
        static const size_t maxLogLineSize = 16*1000;
        char _buffer[maxLogLineSize];
    };

    class MallocFreeOStream : public std::ostream {
        MONGO_DISALLOW_COPYING(MallocFreeOStream);
    public:
        MallocFreeOStream() : ostream(&_buf) {}

        StringData str() const { return _buf.str(); }
        void rewind() { _buf.rewind(); }
    private:
        MallocFreeStreambuf _buf;
    };

    MallocFreeOStream mallocFreeOStream;

    // This guards mallocFreeOStream. While locking a pthread_mutex isn't guaranteed to be
    // signal-safe, this file does it anyway. The assumption is that the main safety risk to locking
    // a mutex is that you could deadlock with yourself. That risk is protected against by only
    // locking the mutex in fatal functions that log then exit. There is a remaining risk that one
    // of these functions recurses (possible if logging segfaults while handing a segfault). This is
    // currently acceptable because if things are that broken, there is little we can do about it.
    //
    // If in the future, we decide to be more strict about posix signal safety, we could switch to
    // an atomic test-and-set loop, possibly with a mechanism for detecting signals raised while
    // handling other signals.
    boost::mutex streamMutex;

    // must hold streamMutex to call
    void writeMallocFreeStreamToLog() {
        logger::globalLogDomain()->append(
            logger::MessageEventEphemeral(curTimeMillis64(),
                                          logger::LogSeverity::Severe(),
                                          getThreadName(),
                                          mallocFreeOStream.str()));
        mallocFreeOStream.rewind();
    }

    // must hold streamMutex to call
    void printSignalAndBacktrace(int signalNum) {
        mallocFreeOStream << "Got signal: " << signalNum << " (" << strsignal(signalNum) << ").\n";
        printStackTrace(mallocFreeOStream);
        writeMallocFreeStreamToLog();
    }

    // this will be called in certain c++ error cases, for example if there are two active
    // exceptions
    void myTerminate() {
        boost::mutex::scoped_lock lk(streamMutex);
        printStackTrace(mallocFreeOStream << "terminate() called.\n");
        writeMallocFreeStreamToLog();
        ::_exit(EXIT_ABRUPT);
    }

    // this gets called when new fails to allocate memory
    void myNewHandler() {
        boost::mutex::scoped_lock lk(streamMutex);
        printStackTrace(mallocFreeOStream << "out of memory.\n");
        writeMallocFreeStreamToLog();
        ::_exit(EXIT_ABRUPT);
    }

    void abruptQuit(int signalNum) {
        boost::mutex::scoped_lock lk(streamMutex);
        printSignalAndBacktrace(signalNum);

        // Don't go through normal shutdown procedure. It may make things worse.
        ::_exit(EXIT_ABRUPT);
    }

#ifdef _WIN32

    void consoleTerminate( const char* controlCodeName ) {
        Client::initThread( "consoleTerminate" );
        log() << "got " << controlCodeName << ", will terminate after current cmd ends" << endl;
        exitCleanly( EXIT_KILL );
    }

    BOOL WINAPI CtrlHandler( DWORD fdwCtrlType ) {

        switch( fdwCtrlType ) {

        case CTRL_C_EVENT:
            log() << "Ctrl-C signal";
            consoleTerminate( "CTRL_C_EVENT" );
            return TRUE ;

        case CTRL_CLOSE_EVENT:
            log() << "CTRL_CLOSE_EVENT signal";
            consoleTerminate( "CTRL_CLOSE_EVENT" );
            return TRUE ;

        case CTRL_BREAK_EVENT:
            log() << "CTRL_BREAK_EVENT signal";
            consoleTerminate( "CTRL_BREAK_EVENT" );
            return TRUE;

        case CTRL_LOGOFF_EVENT:
            // only sent to services, and only in pre-Vista Windows; FALSE means ignore
            return FALSE;

        case CTRL_SHUTDOWN_EVENT:
            log() << "CTRL_SHUTDOWN_EVENT signal";
            consoleTerminate( "CTRL_SHUTDOWN_EVENT" );
            return TRUE;

        default:
            return FALSE;
        }
    }

    void eventProcessingThread() {
        std::string eventName = getShutdownSignalName(ProcessId::getCurrent().asUInt32());

        HANDLE event = CreateEventA(NULL, TRUE, FALSE, eventName.c_str());
        if (event == NULL) {
            warning() << "eventProcessingThread CreateEvent failed: "
                << errnoWithDescription();
            return;
        }

        ON_BLOCK_EXIT(CloseHandle, event);

        int returnCode = WaitForSingleObject(event, INFINITE);
        if (returnCode != WAIT_OBJECT_0) {
            if (returnCode == WAIT_FAILED) {
                warning() << "eventProcessingThread WaitForSingleObject failed: "
                    << errnoWithDescription();
                return;
            }
            else {
                warning() << "eventProcessingThread WaitForSingleObject failed: "
                    << errnoWithDescription(returnCode);
                return;
            }
        }

        Client::initThread("eventTerminate");
        log() << "shutdown event signaled, will terminate after current cmd ends";
        exitCleanly(EXIT_CLEAN);
    }

#else

    void abruptQuitWithAddrSignal( int signalNum, siginfo_t *siginfo, void * ) {
        boost::mutex::scoped_lock lk(streamMutex);

        const char* action = (signalNum == SIGSEGV || signalNum == SIGBUS) ? "access" : "operation";
        mallocFreeOStream << "Invalid " << action << " at address: " << siginfo->si_addr;

        // Writing out message to log separate from the stack trace so at least that much gets
        // logged. This is important because we may get here by jumping to an invalid address which
        // could cause unwinding the stack to break.
        writeMallocFreeStreamToLog();

        printSignalAndBacktrace(signalNum);
        ::_exit(EXIT_ABRUPT);
    }

    // The signals in asyncSignals will be processed by this thread only, in order to
    // ensure the db and log mutexes aren't held. Because this is run in a different thread, it does
    // not need to be safe to call in signal context.
    sigset_t asyncSignals;
    void signalProcessingThread() {
        Client::initThread( "signalProcessingThread" );
        while (true) {
            int actualSignal = 0;
            int status = sigwait( &asyncSignals, &actualSignal );
            fassert(16781, status == 0);
            switch (actualSignal) {
            case SIGUSR1:
                // log rotate signal
                fassert(16782, rotateLogs());
                logProcessDetailsForLogRotate();
                break;
            case SIGQUIT:
                log() << "Received SIGQUIT; terminating.";
                _exit(EXIT_ABRUPT);
            default:
                // interrupt/terminate signal
                log() << "got signal " << actualSignal << " (" << strsignal( actualSignal )
                      << "), will terminate after current cmd ends" << endl;
                exitCleanly( EXIT_CLEAN );
                break;
            }
        }
    }
#endif
} // namespace

    void setupSignalHandlers() {
        set_terminate( myTerminate );
        set_new_handler( myNewHandler );

        // SIGABRT is the only signal we want handled by signal handlers on both windows and posix.
        invariant( signal(SIGABRT, abruptQuit) != SIG_ERR );

#ifdef _WIN32
        _set_purecall_handler( ::abort ); // TODO improve?
        setWindowsUnhandledExceptionFilter();
        massert(10297,
                "Couldn't register Windows Ctrl-C handler",
                SetConsoleCtrlHandler(static_cast<PHANDLER_ROUTINE>(CtrlHandler), TRUE));

#else
        invariant( signal(SIGHUP , SIG_IGN ) != SIG_ERR );
        invariant( signal(SIGUSR2, SIG_IGN ) != SIG_ERR );
        invariant( signal(SIGPIPE, SIG_IGN) != SIG_ERR );

        struct sigaction addrSignals;
        memset( &addrSignals, 0, sizeof( struct sigaction ) );
        addrSignals.sa_sigaction = abruptQuitWithAddrSignal;
        sigemptyset( &addrSignals.sa_mask );
        addrSignals.sa_flags = SA_SIGINFO;

        invariant( sigaction(SIGSEGV, &addrSignals, 0) == 0 );
        invariant( sigaction(SIGBUS, &addrSignals, 0) == 0 );
        invariant( sigaction(SIGILL, &addrSignals, 0) == 0 );
        invariant( sigaction(SIGFPE, &addrSignals, 0) == 0 );


        setupSIGTRAPforGDB();

        // asyncSignals is a global variable listing the signals that should be handled by the
        // interrupt thread, once it is started via startSignalProcessingThread().
        sigemptyset( &asyncSignals );
        sigaddset( &asyncSignals, SIGHUP );
        sigaddset( &asyncSignals, SIGINT );
        sigaddset( &asyncSignals, SIGTERM );
        sigaddset( &asyncSignals, SIGQUIT );
        sigaddset( &asyncSignals, SIGUSR1 );
        sigaddset( &asyncSignals, SIGXCPU );
#endif
    }

    void startSignalProcessingThread() {
#ifdef _WIN32
        boost::thread(eventProcessingThread).detach();
#else
        // Mask signals in the current (only) thread. All new threads will inherit this mask.
        invariant( pthread_sigmask( SIG_SETMASK, &asyncSignals, 0 ) == 0 );
        // Spawn a thread to capture the signals we just masked off.
        boost::thread( signalProcessingThread ).detach();
#endif
    }

} // namespace mongo
