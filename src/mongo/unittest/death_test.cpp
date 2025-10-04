/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/unittest/death_test.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/unittest/test_info.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fmt/format.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if __has_feature(thread_sanitizer)
#include <sanitizer/common_interface_defs.h>
#endif

#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debugger.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/pcre_util.h"
#include "mongo/util/quick_exit.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


#if __has_feature(thread_sanitizer)
#define TSAN_ENABLED_
#endif
#if __has_feature(address_sanitizer)
#define ASAN_ENABLED_
#endif
#if __has_feature(memory_sanitizer)
#define MSAN_ENABLED_
#endif

namespace mongo {
namespace unittest {

class DeathTestSyscallException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define LOG_AND_THROW_WITH_ERRNO(expr) logAndThrowWithErrnoAt(expr, __FILE__, __LINE__)

void logAndThrowWithErrnoAt(StringData expr, StringData file, unsigned line) {
    auto ec = lastPosixError();
    LOGV2_ERROR(24138,
                "expression failed",
                "expr"_attr = expr,
                "error"_attr = errorMessage(ec),
                "file"_attr = file,
                "line"_attr = line);
    breakpoint();
    throw DeathTestSyscallException(
        fmt::format("{} failed: {} @{}:{}", expr, errorMessage(ec), file, line));
}


/**
 * Logs an artifact about why a death test might be skipped.
 * As a side effect, defines the DEATH_TEST_ENABLED macro.
 */
void initDeathTest() {
#if defined(ASAN_ENABLED_) || defined(MSAN_ENABLED_)
    LOGV2(5306900, "Skipping death test in sanitizer build");
#elif defined(_WIN32)
    LOGV2(24133, "Skipping death test on Windows");
#elif defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
    LOGV2(24134, "Skipping death test on tvOS/watchOS");
#else
#define DEATH_TEST_ENABLED
#endif
}

#ifdef DEATH_TEST_ENABLED
struct DeathTestBase::Subprocess {
    void run();
    void execChild(std::string tempPath);
    void monitorChild(FILE* fromChild);
    void prepareChild(int (&pipes)[2]);
    void invokeTest();

    DeathTestBase* death;
    pid_t child;
};
#endif

void DeathTestBase::_doTest() {
    initDeathTest();
#ifdef DEATH_TEST_ENABLED
    Subprocess{this}.run();
#endif  // DEATH_TEST_ENABLED
}

#ifdef DEATH_TEST_ENABLED
#define THROWY_LIBC_IF(expr, isErr)          \
    [&] {                                    \
        auto&& rLocal_{expr};                \
        if (isErr(rLocal_))                  \
            LOG_AND_THROW_WITH_ERRNO(#expr); \
        return rLocal_;                      \
    }()
#define THROWY_LIBC(expr) THROWY_LIBC_IF(expr, [](auto r) { return r == -1; })

namespace {
template <typename F>
int eintrLoop(F&& libcCall) {
    while (true) {
        errno = 0;
        auto&& r{libcCall()};
        if (r == -1 && errno == EINTR)
            continue;
        return r;
    }
}

/**
 * Reassign `arg0` (obtained from an `argv[0]`) with its full path.
 * This is necessary to self-exec if this program was invoked through a `$PATH`
 * lookup, such that `argv[0]` is just a basename. The `execve` used needs a
 * pathname and does not consult the PATH.
 *
 * If `arg0` contains any '/' characters, then it didn't come from a PATH
 * substitution, and is not changed.
 *
 * This reassignment is only attempted on Linux. This is simply because it's
 * easy there to perform a realpath("/proc/self/exe") and find this
 * program's executable pathname. Support for other OSes can be added as needed.
 *
 * Possibilities:
 *   - "Finding current executable's path without /proc/self/exe"
 *     [link](https://stackoverflow.com/questions/1023306)
 *   - "What is the equivalent of /proc/self/exe on Macintosh OS X Mavericks?"
 *     [link](https://stackoverflow.com/questions/22675457)
 */
void canonicalizeExe(std::string& arg0) {
    // If it contains slashes, `orig` is already a pathname. It didn't come
    // from a PATH search.
    if (arg0.find('/') != std::string::npos) {
        LOGV2_DEBUG(
            7070500, 1, "Not changing executable pathname containing '/'", "arg0"_attr = arg0);
        return;
    }
#ifdef __linux__
    char* absPath = realpath("/proc/self/exe", nullptr);
    if (!absPath) {
        auto ec = lastPosixError();
        LOGV2_WARNING(7070501,
                      "Call to realpath failed. Not changing arg0",
                      "arg0"_attr = arg0,
                      "error"_attr = errorMessage(ec));
        return;
    }
    ScopeGuard freeGuard = [&] {
        free(absPath);
    };
    auto orig = std::exchange(arg0, std::string(absPath));
    LOGV2_INFO(7070502, "Canonical executable pathname", "orig"_attr = orig, "arg0"_attr = arg0);
#else
    LOGV2_DEBUG(7070503, 1, "Recovery of executable pathname unimplemented", "arg0"_attr = arg0);
#endif
}

/** Removes "--opt val" and "--opt=val" argument sequences from `av`. */
void stripOption(std::vector<std::string>& av, StringData opt) {
    for (size_t i = 0; i < av.size();) {
        StringData sd = av[i];
        if (sd == fmt::format("--{}", opt)) {
            if (i + 1 < av.size())
                av.erase(av.begin() + i, av.begin() + i + 2);
        } else if (sd.starts_with(fmt::format("--{}=", opt))) {
            av.erase(av.begin() + i);
        } else {
            ++i;
        }
    }
}

}  // namespace

void DeathTestBase::Subprocess::run() {
    if (!getSpawnInfo().internalRunDeathTest.empty()) {
        invokeTest();  // We're in an execve child process.
        return;
    }

    // There are a few reasons to fall back to non-exec death tests.
    // These are mostly unusual tests with a custom main.
    bool doExec = death->_exec;
    if (!UnitTest::getInstance()->currentTestInfo()) {
        LOGV2(6186002, "Cannot exec child without currentTestInfo");
        doExec = false;
    }
    if (!getSpawnInfo().deathTestExecAllowed) {
        LOGV2(6186003, "Death test exec disallowed");
        doExec = false;
    }
    if (getSpawnInfo().argVec.empty()) {
        LOGV2(6186004, "Cannot exec child without an argVec");
        doExec = false;
    }
    LOGV2(6186001, "Child", "exec"_attr = doExec);

    TempDir childTempPath{"DeathTestChildTempPath"};

    int pipes[2];
    THROWY_LIBC(pipe(pipes));
    if ((child = THROWY_LIBC(fork())) != 0) {
        THROWY_LIBC(close(pipes[1]));
        FILE* pf = THROWY_LIBC_IF(fdopen(pipes[0], "r"), [](FILE* f) { return !f; });
        ScopeGuard pfGuard = [&] {
            THROWY_LIBC(fclose(pf));
        };
        monitorChild(pf);
    } else {
        prepareChild(pipes);
        if (doExec) {
            // Go further: fully reboot the child with `execve`.
            execChild(childTempPath.release());
        } else {
            TempDir::setTempPath(childTempPath.release());
            invokeTest();
        }
    }
}

void DeathTestBase::Subprocess::execChild(std::string tempPath) {
    auto& spawnInfo = getSpawnInfo();
    std::vector<std::string> av = spawnInfo.argVec;
    canonicalizeExe(av[0]);
    // Arrange for the subprocess to execute only this test, exactly once.
    // Remove '--repeat' option. We want to repeat the whole death test not its child.
    stripOption(av, "repeat");
    stripOption(av, "suite");
    stripOption(av, "filter");
    stripOption(av, "filterFileName");
    stripOption(av, "tempPath");
    const TestInfo* info = UnitTest::getInstance()->currentTestInfo();
    av.push_back(fmt::format("--suite={}", info->suiteName()));
    av.push_back(fmt::format("--filter=^{}$", pcre_util::quoteMeta(info->testName())));
    av.push_back(fmt::format("--tempPath={}", tempPath));
    // The presence of this flag is how the test body in the child process knows it's in the
    // child process, and therefore to not exec again. Its value is ignored.
    av.push_back("--internalRunDeathTest=1");

    LOGV2(6186000, "Exec", "argv"_attr = av);

    std::vector<char*> avp;
    std::transform(av.begin(), av.end(), std::back_inserter(avp), [](auto& s) { return s.data(); });
    avp.push_back(nullptr);
    THROWY_LIBC(execv(avp.front(), avp.data()));
}

void DeathTestBase::Subprocess::monitorChild(FILE* pf) {
    std::ostringstream os;

    LOGV2(5042601, "Death test starting");
    ScopeGuard alwaysLogExit = [] {
        LOGV2(5042602, "Death test finishing");
    };

    char* lineBuf = nullptr;
    size_t lineBufSize = 0;
    ScopeGuard lineBufGuard = [&] {
        free(lineBuf);
    };
    while (true) {
        ssize_t bytesRead = eintrLoop([&] { return getline(&lineBuf, &lineBufSize, pf); });
        if (bytesRead == -1)
            break;
        StringData line(lineBuf, bytesRead);
        if (line.empty())
            continue;
        if (line[line.size() - 1] == '\n')
            line = line.substr(0, line.size() - 1);
        if (line.empty())
            continue;
        try {
            auto parsedChildLog = fromjson(lineBuf);
            LOGV2(20165, "child", "json"_attr = parsedChildLog);
        } catch (DBException&) {
            // ignore json parsing errors and dump the whole log line as text
            LOGV2(20169, "child", "text"_attr = line);
        }
        os.write(lineBuf, bytesRead);
        invariant(os);
    }
    if (!feof(pf))
        LOG_AND_THROW_WITH_ERRNO("getline(&buf, &bufSize, pf)");

    int stat;
    THROWY_LIBC(eintrLoop([&] { return waitpid(child, &stat, 0); }));

    if (WIFSIGNALED(stat) || (WIFEXITED(stat) && WEXITSTATUS(stat) != 0)) {
        // Exited with a signal or non-zero code. Validate the expected message.
#if defined(TSAN_ENABLED_)
        if (WEXITSTATUS(stat) == static_cast<int>(ExitCode::threadSanitizer)) {
            FAIL(
                "Death test exited with Thread Sanitizer exit code, search test output for "
                "'ThreadSanitizer' for more information");
        }
#endif
        if (death->_isRegex()) {
            ASSERT_STRING_SEARCH_REGEX(os.str(), death->_doGetPattern())
                << " @" << death->_getFile() << ":" << death->_getLine();
        } else {
            ASSERT_STRING_CONTAINS(os.str(), death->_doGetPattern())
                << " @" << death->_getFile() << ":" << death->_getLine();
        }
        LOGV2(5042603, "Death test test died as expected");
        return;
    } else {
        invariant(!WIFSTOPPED(stat));
    }
    FAIL("Expected death, found life\n\n") << os.str();
}

void DeathTestBase::Subprocess::prepareChild(int (&pipes)[2]) {
    THROWY_LIBC(close(pipes[0]));
    THROWY_LIBC(dup2(pipes[1], STDOUT_FILENO));
    THROWY_LIBC(dup2(pipes[1], STDERR_FILENO));
    THROWY_LIBC(close(pipes[1]));
    THROWY_LIBC(close(STDIN_FILENO));

    // We disable the creation of core dump files in the child process since the child process
    // is expected to exit uncleanly. This avoids unnecessarily creating core dump files when
    // the child process calls std::abort() or std::terminate().
    const struct rlimit zeroLimit = {0, 0};
    THROWY_LIBC(setrlimit(RLIMIT_CORE, &zeroLimit));

#if defined(TSAN_ENABLED_)
    // Our callback handler exits with the default TSAN exit code so we can check in the death test
    // framework Without this, the use could override the exit code and get a false positive that
    // the test passes in TSAN builds.
    __sanitizer_set_death_callback(+[] { _exit(static_cast<int>(ExitCode::threadSanitizer)); });
#endif
}

void DeathTestBase::Subprocess::invokeTest() {
    try {
        auto test = death->_doMakeTest();
        LOGV2(23515, "Running DeathTest in child");
        test->run();
        LOGV2(20166, "Death test failed to die");
    } catch (const TestAssertionFailureException& tafe) {
        LOGV2(24137, "Death test threw test exception instead of dying", "exception"_attr = tafe);
    } catch (...) {
        LOGV2(20167, "Death test threw exception instead of dying");
    }
    // To fail the test, we must exit with a successful error code, because the parent process
    // is checking for the child to die with an exit code indicating an error.
    quickExit(ExitCode::clean);
}
#endif  // DEATH_TEST_ENABLED

}  // namespace unittest
}  // namespace mongo
