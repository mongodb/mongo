/**
 * CI Wrapper Child Timeout Test
 *
 * Reproduces the scenario from DEVPROD-27556: the unit test framework forks a child process for
 * test isolation, the child hangs, and only the parent (waiting on the child) would previously be
 * cored on timeout. The fix (DEVPROD-27556) signals the whole process group so both parent and
 * child are cored.
 *
 * When run via --run_under=//bazel:test_wrapper this test should:
 *   - Fork a child that sleeps longer than the timeout
 *   - Have the parent wait on the child (simulating a test framework waiting for its worker)
 *   - Get a SIGABRT to the entire process group after the timeout, producing cores from both
 */

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include <unistd.h>

#include <sys/wait.h>

namespace mongo {
namespace {

constexpr int kChildSleepSeconds = 700;

TEST(CIWrapperChildTimeoutTest, ShouldCaptureChildCoreOnTimeout) {
    pid_t child = fork();
    ASSERT_NE(child, pid_t{-1}) << "fork() failed: " << strerror(errno);

    if (child == 0) {
        // Child: sleep well beyond the timeout to simulate a stuck test worker.
        // The interesting state (e.g. the hung query or deadlock) lives here, not in the parent.
        std::cout << "Child process (PID " << getpid() << ") sleeping for " << kChildSleepSeconds
                  << "s to simulate a stuck test" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(kChildSleepSeconds));
        _exit(0);
    }

    // Parent: block in waitpid, simulating a test framework waiting for its worker process.
    // Before the DEVPROD-27556 fix, only this (boring) parent process was cored on timeout.
    std::cout << "Parent (PID " << getpid() << ") waiting on child PID " << child << std::endl;
    int status;
    waitpid(child, &status, 0);

    FAIL("Test should have been killed by SIGABRT to the process group before reaching this point");
}

}  // namespace
}  // namespace mongo
