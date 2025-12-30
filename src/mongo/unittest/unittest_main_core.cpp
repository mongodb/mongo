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
#include "mongo/unittest/unittest_main_core.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/enhanced_reporter.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_details.h"
#include "mongo/unittest/unittest_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/pcre.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

#include <boost/filesystem.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::unittest {

static EnhancedReporter* gEnhancedReporter = nullptr;

EnhancedReporter* getGlobalEnhancedReporter() {
    return gEnhancedReporter;
}

namespace {

void _dumpOutputSignalHandlerCb() {
    gEnhancedReporter->dumpBufferedOutputForSignalHandler();
}

/** Sets and resets the FCV as each test starts and ends. */
class FCVEventListener : public testing::EmptyTestEventListener {
public:
    void OnTestStart(const testing::TestInfo&) override {
        // Attempting to read the featureCompatibilityVersion parameter before it is explicitly
        // initialized with a meaningful value will trigger failures as of SERVER-32630.
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
    }

    void OnTestEnd(const testing::TestInfo&) override {
        serverGlobalParams.mutableFCV.reset();
    }
};

class ThrowListener : public testing::EmptyTestEventListener {
    void OnTestPartResult(const testing::TestPartResult& result) override {
        if (result.type() != testing::TestPartResult::kFatalFailure)
            return;

        StringData msg = result.message();
        // Try to avoid throwing an exception when reporting that an unexpected C++ exception
        // was thrown. That is because we are already in the top-level block, and if we throw
        // an exception here, it won't be able to be caught by the same block.
        // See ::testing::internal::FormatCxxExceptionMessage() for the format of the message,
        // and ::testing::internal::HandleExceptionsInMethodIfSupported() for the try/catch.
        bool unexpectedException = std::uncaught_exceptions() != 0 ||
            (std::current_exception() &&
             (msg.starts_with("C++ exception with description") ||
              msg.starts_with("Unknown C++ exception")) &&
             msg.contains(" thrown in "));
        if (!unexpectedException)
            throw testing::AssertionException(result);
    }
};

void forEachTest(const auto& f) {
    auto inst = testing::UnitTest::GetInstance();
    for (int si = 0; si != inst->total_test_suite_count(); ++si) {
        auto& suite = *inst->GetTestSuite(si);
        for (int ti = 0; ti != suite.total_test_count(); ++ti) {
            const auto& testInfo = *suite.GetTestInfo(ti);
            f(suite.name(), testInfo.name());
        }
    }
}

std::vector<const testing::TestSuite*> allSuites() {
    std::vector<const testing::TestSuite*> out;
    auto inst = testing::UnitTest::GetInstance();
    for (int si = 0; si != inst->total_test_suite_count(); ++si)
        out.push_back(inst->GetTestSuite(si));
    return out;
}

/** The only special character is `*`. */
bool matchesGooglePattern(StringData p, StringData s) {
    if (p.empty())
        return s.empty();
    if (p.front() == '*') {
        if (matchesGooglePattern(p.substr(1), s.substr(1)))
            return true;
        return matchesGooglePattern(p, s.substr(1));
    }
    if (p.front() == s.front())
        return matchesGooglePattern(p.substr(1), s.substr(1));
    return false;
}

bool matchesGoogleFilter(StringData filt, StringData suite, StringData test) {
    std::string fullName = fmt::format("{}.{}", suite, test);
    while (!filt.empty()) {
        if (filt.front() == ':') {
            filt.remove_prefix(1);
            continue;
        }
        size_t pos = filt.find_first_of(":");
        StringData elem = filt.substr(0, pos);
        filt = filt.substr(pos);
        if (matchesGooglePattern(elem, fullName))
            return true;
    }
    return false;
}

std::string pruneGoogleFilter(const std::string& googleFilter) {
    std::string out = googleFilter;
    forEachTest([&](const auto& suite, const auto& test) { ; });
    return out;
}

void applyTestFilters(const std::vector<std::string>& suites,
                      const std::string& filter,
                      const std::string& fileNameFilter) {
    boost::optional<pcre::Regex> filterRe;
    boost::optional<pcre::Regex> fileNameFilterRe;
    if (!filter.empty())
        filterRe.emplace(filter);
    if (!fileNameFilter.empty())
        fileNameFilterRe.emplace(fileNameFilter);

    std::vector<SelectedTest> selection;
    auto inst = testing::UnitTest::GetInstance();
    for (int si = 0; si != inst->total_test_suite_count(); ++si) {
        auto& suite = *inst->GetTestSuite(si);
        bool keepSuite = suites.empty() ||
            std::any_of(suites.begin(), suites.end(), [&](auto&& s) { return suite.name() == s; });
        for (int ti = 0; ti != suite.total_test_count(); ++ti) {
            const auto& testInfo = *suite.GetTestInfo(ti);
            bool keep = true;
            if (!keepSuite)
                keep = false;
            if (fileNameFilterRe && !fileNameFilterRe->matchView(testInfo.file()))
                keep = false;
            if (filterRe && !filterRe->matchView(testInfo.name()))
                keep = false;
            selection.push_back({suite.name(), testInfo.name(), keep});
        }
    }
    GTEST_FLAG_SET(filter, gtestFilterForSelection(selection));
}

bool isDeathTestChild() {
    return !GTEST_FLAG_GET(internal_run_death_test).empty();
}

void initializeDeathTestChild() {
    if (GTEST_FLAG_GET(internal_run_death_test).empty())
        return;
    // (Generic FCV reference): test-only
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

#ifdef MONGO_CONFIG_DEV_STACKTRACE
    disableDevStackTrace();
#endif
    details::suppressCoreDumps();
    details::redirectStdoutToStderr();
    ::testing::UnitTest::GetInstance()->listeners().SuppressEventForwarding(false);
}

void installEnhancedReporter(EnhancedReporter::Options options) {
    auto& listeners = testing::UnitTest::GetInstance()->listeners();
    auto* defaultListener = listeners.default_result_printer();
    invariant(defaultListener, "GoogleTest default listener already removed.");
    std::unique_ptr<testing::TestEventListener> originalPrinter{listeners.Release(defaultListener)};
    auto enhanced =
        std::make_unique<EnhancedReporter>(std::move(originalPrinter), std::move(options));
    gEnhancedReporter = enhanced.get();
    setSynchronousSignalHandlerCallback_forTest(_dumpOutputSignalHandlerCb);
    setSignalPostProcessingCallback_forTest(_dumpOutputSignalHandlerCb);
    listeners.Append(enhanced.release());
}

/**
 * Convert the vector to C-style argv to call google init functions. These might consume
 * some elements, so we rebuild the vector after.
 */
void callInitGoogleTest(std::vector<std::string>& argVec) {
    int argc = argVec.size();
    auto ownedArgv = std::make_unique<const char*[]>(argc + 1);
    for (int i = 0; i < argc; ++i)
        ownedArgv[i] = argVec[i].c_str();
    ownedArgv[argc] = nullptr;
    auto argv = const_cast<char**>(ownedArgv.get());
    testing::InitGoogleTest(&argc, argv);
    testing::InitGoogleMock(&argc, argv);
    for (int i = 0; i < argc; ++i)
        argVec[i] = argv[i];
    argVec.resize(argc);
}

}  // namespace

std::string gtestFilterForSelection(const std::vector<SelectedTest>& selection) {
    std::string filt;
    StringData sep;
    for (const auto& [s, t, k] : selection) {
        if (k)
            filt += fmt::format("{}{}.{}", std::exchange(sep, ":"_sd), s, t);
    }
    return filt;
}

void MainProgress::initialize() {
    callInitGoogleTest(_argVec);

    // Colorize when explicitly asked to. If no position is taken, colorize when we are writing
    // to a TTY.
    if (details::gtestColorDefaulted() && details::stdoutIsTty()) {
        GTEST_FLAG_SET(color, "yes");
    }

    if (isDeathTestChild()) {
        initializeDeathTestChild();
        return;
    }

    // Googletest takes ownership of the listener.
    testing::UnitTest::GetInstance()->listeners().Append(new FCVEventListener{});
}

boost::optional<ExitCode> MainProgress::parseAndAcceptOptions() {
    auto uto = parseUnitTestOptions(args());
    if (uto.help) {
        std::cerr << getUnitTestOptionsHelpString(_argVec) << std::endl;
        return ExitCode::clean;
    }

    if (uto.list && *uto.list) {
        for (auto&& s : allSuites())
            std::cout << s->name() << std::endl;
        return ExitCode::clean;
    }

    if (_options.startSignalProcessingThread) {
        // Per SERVER-7434, startSignalProcessingThread must run after any forks (i.e.
        // initialize_server_global_state::forkServerOrDie) and before the creation of any other
        // threads
        startSignalProcessingThread();
    }

    if (uto.verbose) {
        if (uto.verbose->find_first_not_of("v") != std::string::npos) {
            std::cerr
                << "The string for the --verbose option cannot contain characters other than 'v'"
                << std::endl
                << optionenvironment::startupOptions.helpString();
            return ExitCode::fail;
        }
        setMinimumLoggedSeverity(logv2::LogSeverity::Debug(uto.verbose->size()));
    }

    // Transitional: Treat `--repeat n` as a synonym for `--gtest_repeat=n`.
    if (uto.repeat && *uto.repeat != 1)
        GTEST_FLAG_SET(repeat, *uto.repeat);

    // We allow options from the unit test's caller (i.e. main) to override the filter flags.
    LOGV2_DEBUG(10947000,
                1,
                "Filter option overrides",
                "suites"_attr = _options.testSuites,
                "filter"_attr = _options.testFilter,
                "fileNameFilter"_attr = _options.fileNameFilter);

    if (auto&& o = _options.testSuites)
        uto.suites = *o;
    if (auto&& o = _options.testFilter)
        uto.filter = *o;
    if (auto&& o = _options.fileNameFilter)
        uto.fileNameFilter = *o;

    applyTestFilters(uto.suites.value_or(std::vector<std::string>{}),
                     uto.filter.value_or(""),
                     uto.fileNameFilter.value_or(""));

    if (uto.tempPath)
        TempDir::setTempPath(*uto.tempPath);

    {
        AutoUpdateConfig auc;
        if (auto&& o = uto.autoUpdateAsserts)
            auc.updateFailingAsserts = *o;
        if (auto&& o = uto.rewriteAllAutoAsserts)
            auc.revalidateAll = *o;
        if (auc.revalidateAll && !auc.updateFailingAsserts) {
            std::cerr << "`--rewriteAllAutoAsserts` requires `--autoUpdateAsserts`.\n";
            return ExitCode::fail;
        }
        auto&& exe = _argVec.front();
        auc.executablePath = boost::filesystem::canonical(boost::filesystem::path(exe));
        getAutoUpdateConfig() = std::move(auc);
    }

    if (uto.enhancedReporter.value_or(true)) {
        if (!isDeathTestChild()) {
            EnhancedReporter::Options ero;
            ero.showEachTest = uto.showEachTest.value_or(ero.showEachTest);
            installEnhancedReporter(ero);
        }
    }
    return {};
}

int MainProgress::test() {
    clearSignalMask();
    setupSynchronousSignalHandlers();

    if (!TestingProctor::instance().isInitialized()) {
        TestingProctor::instance().setEnabled(true);
    }
    setTestCommandsEnabled(true);

    if (!_options.suppressGlobalInitializers) {
        runGlobalInitializersOrDie(_argVec);
    }

    testing::UnitTest::GetInstance()->listeners().Append(new ThrowListener{});

    auto result = RUN_ALL_TESTS();

    if (!_options.suppressGlobalInitializers) {
        if (auto ret = runGlobalDeinitializers(); !ret.isOK()) {
            std::cerr << "Global deinitialization failed: " << ret.reason() << std::endl;
        }
    }

    TestingProctor::instance().exitAbruptlyIfDeferredErrors();

    return result;
}

}  // namespace mongo::unittest
