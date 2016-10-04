/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/unittest/death_test.h"

#ifndef _WIN32
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <sstream>

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"

#define checkSyscall(EXPR)                                              \
    do {                                                                \
        if (-1 == (EXPR)) {                                             \
            const int err = errno;                                      \
            severe() << #EXPR " failed: " << errnoWithDescription(err); \
            invariantFailed("-1 != (" #EXPR ")", __FILE__, __LINE__);   \
        }                                                               \
    } while (false)

namespace mongo {
namespace unittest {

DeathTestImpl::DeathTestImpl(std::unique_ptr<Test> test) : _test(std::move(test)) {}

void DeathTestImpl::_doTest() {
#ifdef _WIN32
    log() << "Skipping death test on Windows";
    return;
#else
    int pipes[2];
    checkSyscall(pipe(pipes));
    pid_t child;
    checkSyscall(child = fork());
    if (child) {
        checkSyscall(close(pipes[1]));
        char buf[1000];
        std::ostringstream os;
        ssize_t bytesRead;
        while (0 < (bytesRead = read(pipes[0], buf, sizeof(buf)))) {
            os.write(buf, bytesRead);
            invariant(os);
        }
        checkSyscall(bytesRead);
        pid_t pid;
        int stat;
        while (child != (pid = waitpid(child, &stat, 0))) {
            invariant(pid == -1);
            const int err = errno;
            switch (err) {
                case EINTR:
                    continue;
                default:
                    severe() << "Unrecoverable error while waiting for " << child << ": "
                             << errnoWithDescription(err);
                    MONGO_UNREACHABLE;
            }
        }
        if (WIFSIGNALED(stat) || (WIFEXITED(stat) && WEXITSTATUS(stat) != 0)) {
            // Exited with a signal or non-zero code.  Should check the pattern, here,
            // but haven't figured out how, so just return.
            ASSERT_STRING_CONTAINS(os.str(), getPattern());
            return;
        } else {
            invariant(!WIFSTOPPED(stat));
        }
        FAIL("Expected death, found life\n\n") << os.str();
    }

    // This code only executes in the child process.
    checkSyscall(close(pipes[0]));
    checkSyscall(dup2(pipes[1], 1));
    checkSyscall(dup2(1, 2));
    try {
        _test->run();
    } catch (const TestAssertionFailureException& tafe) {
        log() << "Caught test exception while expecting death: " << tafe;
        // To fail the test, we must exit with a successful error code, because the parent process
        // is checking for the child to die with an exit code indicating an error.
        quickExit(EXIT_SUCCESS);
    }
    quickExit(EXIT_SUCCESS);
#endif
}

}  // namespace unittest
}  // namespace mongo
