/**
 * CI Wrapper Timeout Test
 *
 * This test is designed to verify the timeout behavior of test_wrapper.sh.
 * It intentionally sleeps longer than the configured timeout (10 minutes) to:
 * 1. Trigger the SIGABRT signal from test_wrapper.sh
 * 2. Generate a coredump file
 *
 * When run via the --run_under=//bazel:test_wrapper param, this test should:
 * - Get killed by SIGABRT after 10 minutes (or 40 on ppc64le/s390x)
 * - Produce a coredump file that gets saved to TEST_UNDECLARED_OUTPUTS_DIR
 *
 * Usage:
 *   bazel test --config=remote_test
 * //buildscripts/bazel_testbuilds:ci_wrapper_timeout_test --run_under=//bazel:test_wrapper
 */

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace mongo {
namespace {

// Sleep duration that exceeds the test_wrapper.sh timeout (600 second default)
constexpr int kSleepSeconds = 610;

TEST(CIWrapperTimeoutTest, ShouldTimeoutAndGenerateCoredump) {
    std::cout << "Starting timeout test - will sleep for " << kSleepSeconds << " seconds"
              << std::endl;
    std::cout << "Expected: test_wrapper.sh should send SIGABRT after 600 second" << std::endl;
    std::cout << "This should trigger a coredump if ulimit -c is set correctly" << std::endl;

    // Sleep longer than the timeout to trigger SIGABRT
    std::this_thread::sleep_for(std::chrono::seconds(kSleepSeconds));

    // If we reach here, the timeout mechanism didn't work
    FAIL("Test should have been killed by SIGABRT before reaching this point");
}

}  // namespace
}  // namespace mongo

