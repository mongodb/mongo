// framework.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/dbtests/framework.h"

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/dur.h"
#include "mongo/db/ops/update.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/dbtests/framework_options.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/version.h"

namespace moe = mongo::optionenvironment;

namespace mongo {

    namespace dbtests {

        mutex globalCurrentTestNameMutex("globalCurrentTestNameMutex");
        std::string globalCurrentTestName;

        class TestWatchDog : public BackgroundJob {
        public:
            virtual string name() const { return "TestWatchDog"; }
            virtual void run(){

                int minutesRunning = 0;
                std::string lastRunningTestName, currentTestName;

                {
                    scoped_lock lk( globalCurrentTestNameMutex );
                    lastRunningTestName = globalCurrentTestName;
                }

                while (true) {
                    sleepsecs(60);
                    minutesRunning++;

                    {
                        scoped_lock lk( globalCurrentTestNameMutex );
                        currentTestName = globalCurrentTestName;
                    }

                    if (currentTestName != lastRunningTestName) {
                        minutesRunning = 0;
                        lastRunningTestName = currentTestName;
                    }

                    if (minutesRunning > 30){
                        log() << currentTestName << " has been running for more than 30 minutes. aborting." << endl;
                        ::abort();
                    }
                    else if (minutesRunning > 1){
                        warning() << currentTestName << " has been running for more than " << minutesRunning-1 << " minutes." << endl;
                    }
                }
            }
        };

MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                            ("GlobalLogManager"),
                            ("default", "completedStartupConfig"))(InitializerContext* context) {

    frameworkOptions = moe::OptionSection("options");
    moe::OptionsParser parser;

    Status retStatus = addTestFrameworkOptions(&frameworkOptions);
    if (!retStatus.isOK()) {
        return retStatus;
    }

    retStatus = parser.run(frameworkOptions, context->args(), context->env(),
                           &frameworkParsedOptions);
    if (!retStatus.isOK()) {
        std::cerr << retStatus.reason() << std::endl;
        std::cerr << "try '" << context->args()[0]
                  << " --help' for more information" << std::endl;
        ::_exit(EXIT_BADOPTIONS);
    }

    retStatus = preValidationTestFrameworkOptions(frameworkParsedOptions, context->args());
    if (!retStatus.isOK()) {
        return retStatus;
    }

    retStatus = frameworkParsedOptions.validate();
    if (!retStatus.isOK()) {
        return retStatus;
    }

    retStatus = storeTestFrameworkOptions(frameworkParsedOptions, context->args());
    if (!retStatus.isOK()) {
        return retStatus;
    }

    return Status::OK();
}

        int runDbTests(int argc, char** argv) {
            frameworkGlobalParams.perfHist = 1;
            frameworkGlobalParams.seed = time( 0 );
            frameworkGlobalParams.runsPerTest = 1;

            Client::initThread("testsuite");
            acquirePathLock();

            srand( (unsigned) frameworkGlobalParams.seed );
            printGitVersion();
            printOpenSSLVersion();
            printSysInfo();

            FileAllocator::get()->start();

            dur::startup();

            TestWatchDog twd;
            twd.go();

            // set tlogLevel to -1 to suppress MONGO_TLOG(0) output in a test program
            tlogLevel = -1;

            int ret = ::mongo::unittest::Suite::run(frameworkGlobalParams.suites,
                                                    frameworkGlobalParams.filter,
                                                    frameworkGlobalParams.runsPerTest);

#if !defined(_WIN32) && !defined(__sunos__)
            flock( lockFile, LOCK_UN );
#endif

            cc().shutdown();
            dbexit( (ExitCode)ret ); // so everything shuts down cleanly
            return ret;
        }
    }  // namespace dbtests

}  // namespace mongo

void mongo::unittest::onCurrentTestNameChange( const std::string &testName ) {
    scoped_lock lk( mongo::dbtests::globalCurrentTestNameMutex );
    mongo::dbtests::globalCurrentTestName = testName;
}
