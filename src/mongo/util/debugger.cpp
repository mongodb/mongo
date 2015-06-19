// debug_util.cpp

/*    Copyright 2009 10gen Inc.
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

#include "mongo/util/debugger.h"

#include "mongo/db/server_options.h"
#include "mongo/util/debug_util.h"

#ifndef _WIN32
#include <signal.h>
#endif

namespace mongo {
void breakpoint() {
#ifdef _WIN32
    DEV DebugBreak();
#endif
#ifndef _WIN32
    // code to raise a breakpoint in GDB
    ONCE {
        // prevent SIGTRAP from crashing the program if default action is specified and we are not
        // in gdb
        struct sigaction current;
        sigaction(SIGTRAP, NULL, &current);
        if (current.sa_handler == SIG_DFL) {
            signal(SIGTRAP, SIG_IGN);
        }
    }

    raise(SIGTRAP);
#endif
}

#if defined(USE_GDBSERVER)

/* Magic gdb trampoline
 * Do not call directly! call setupSIGTRAPforGDB()
 * Assumptions:
 *  1) gdbserver is on your path
 *  2) You have run "handle SIGSTOP noprint" in gdb
 *  3) serverGlobalParams.port + 2000 is free
 */
void launchGDB(int) {
    // Don't come back here
    signal(SIGTRAP, SIG_IGN);

    const int newPort = serverGlobalParams.port + 2000;

    char pidToDebug[16];
    int pidRet = snprintf(pidToDebug, sizeof(pidToDebug), "%d", getpid());
    invariant(pidRet >= 0 && size_t(pidRet) < sizeof(pidToDebug));

    char hostPort[32];
    int hostRet = snprintf(hostPort, sizeof(hostPort), "localhost:%d", newPort);
    invariant(hostRet >= 0 && size_t(hostRet) < sizeof(hostPort));

    char msg[128];
    int msgRet =
        snprintf(msg, sizeof(msg), "\n\n\t**** Launching gdbserver on %s ****\n\n", hostPort);
    invariant(msgRet >= 0 && size_t(msgRet) < sizeof(msg));
    invariant(write(STDERR_FILENO, msg, msgRet) == msgRet);

    if (fork() == 0) {
        // child
        execlp("gdbserver", "gdbserver", "--attach", hostPort, pidToDebug, NULL);
        perror(NULL);
        _exit(1);
    } else {
        // parent
        raise(SIGSTOP);  // pause all threads until gdb connects and continues
        raise(SIGTRAP);  // break inside gdbserver
    }
}

void setupSIGTRAPforGDB() {
    verify(signal(SIGTRAP, launchGDB) != SIG_ERR);
}
#else
void setupSIGTRAPforGDB() {}
#endif
}
