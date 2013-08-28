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

#include <boost/filesystem/operations.hpp>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/dur.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/new_find.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/version.h"

namespace moe = mongo::optionenvironment;

namespace mongo {

    CmdLine cmdLine;
    moe::OptionSection options;
    moe::Environment params;

    namespace dbtests {

        mutex globalCurrentTestNameMutex("globalCurrentTestNameMutex");
        std::string globalCurrentTestName;

        std::string get_help_string(const StringData& name, const moe::OptionSection& options) {
            StringBuilder sb;
            sb << "usage: " << name << " [options] [suite]...\n"
               << options.helpString() << "suite: run the specified test suite(s) only\n";
            return sb.str();
        }

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

        unsigned perfHist = 1;

        Status addTestFrameworkOptions(moe::OptionSection* options) {

            typedef moe::OptionDescription OD;
            typedef moe::PositionalOptionDescription POD;

            Status ret = options->addOption(OD("help", "help,h", moe::Switch,
                        "show this usage information", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("dbpath", "dbpath", moe::String,
                        "db data path for this test run. NOTE: the contents of this directory will "
                        "be overwritten if it already exists", true,
                        moe::Value(default_test_dbpath)));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("debug", "debug", moe::Switch,
                        "run tests with verbose output", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("list", "list,l", moe::Switch, "list available test suites",
                        true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("bigfiles", "bigfiles", moe::Switch,
                        "use big datafiles instead of smallfiles which is the default", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("filter", "filter,f", moe::String,
                        "string substring filter on test name" , true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("verbose", "verbose,v", moe::Switch, "verbose", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("useNewQueryFramework", "useNewQueryFramework", moe::Switch,
                        "use the new query framework", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("dur", "dur", moe::Switch,
                        "enable journaling (currently the default)", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("nodur", "nodur", moe::Switch, "disable journaling", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("seed", "seed", moe::UnsignedLongLong, "random number seed",
                        true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("runs", "runs", moe::Int,
                        "number of times to run each test", true));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("perfHist", "perfHist", moe::Unsigned,
                        "number of back runs of perf stats to display", true));
            if (!ret.isOK()) {
                return ret;
            }

            ret = options->addOption(OD("suites", "suites", moe::StringVector, "test suites to run",
                        false));
            if (!ret.isOK()) {
                return ret;
            }
            ret = options->addOption(OD("nopreallocj", "nopreallocj", moe::Switch,
                        "disable journal prealloc", false));
            if (!ret.isOK()) {
                return ret;
            }

            ret = options->addPositionalOption(POD("suites", moe::String, -1));
            if (!ret.isOK()) {
                return ret;
            }

            return Status::OK();
        }

MONGO_INITIALIZER_GENERAL(ParseStartupConfiguration,
                            ("GlobalLogManager"),
                            ("default", "completedStartupConfig"))(InitializerContext* context) {

    options = moe::OptionSection("options");
    moe::OptionsParser parser;

    Status retStatus = addTestFrameworkOptions(&options);
    if (!retStatus.isOK()) {
        return retStatus;
    }

    retStatus = parser.run(options, context->args(), context->env(), &params);
    if (!retStatus.isOK()) {
        StringBuilder sb;
        sb << "Error parsing options: " << retStatus.toString() << "\n";
        sb << get_help_string(context->args()[0], options);
        return Status(ErrorCodes::FailedToParse, sb.str());
    }

    return Status::OK();
}

        int runDbTests(int argc, char** argv) {
            unsigned long long seed = time( 0 );
            int runsPerTest = 1;
            string dbpathSpec;

            if (params.count("help")) {
                std::cout << get_help_string(argv[0], options) << std::endl;
                return EXIT_CLEAN;
            }

            if (params.count("useNewQueryFramework")) {
                mongo::enableNewQueryFramework();
            }

            if (params.count("dbpath")) {
                dbpathSpec = params["dbpath"].as<string>();
            }

            if (params.count("seed")) {
                seed = params["seed"].as<unsigned long long>();
            }

            if (params.count("runs")) {
                runsPerTest = params["runs"].as<int>();
            }

            if (params.count("perfHist")) {
                perfHist = params["perfHist"].as<unsigned>();
            }

            bool nodur = false;
            if( params.count("nodur") ) {
                nodur = true;
                cmdLine.dur = false;
            }
            if( params.count("dur") || cmdLine.dur ) {
                cmdLine.dur = true;
            }

            if( params.count("nopreallocj") ) {
                cmdLine.preallocj = false;
            }

            if (params.count("debug") || params.count("verbose") ) {
                logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogSeverity::Debug(1));
            }

            if (params.count("list")) {
                std::vector<std::string> suiteNames = mongo::unittest::getAllSuiteNames();
                for ( std::vector<std::string>::const_iterator i = suiteNames.begin();
                      i != suiteNames.end(); ++i ) {

                    std::cout << *i << std::endl;
                }
                return 0;
            }

            boost::filesystem::path p(dbpathSpec);

            /* remove the contents of the test directory if it exists. */
            if (boost::filesystem::exists(p)) {
                if (!boost::filesystem::is_directory(p)) {
                    std::cerr << "ERROR: path \"" << p.string() << "\" is not a directory"
                              << std::endl;
                    std::cerr << get_help_string(argv[0], options) << std::endl;
                    return EXIT_BADOPTIONS;
                }
                boost::filesystem::directory_iterator end_iter;
                for (boost::filesystem::directory_iterator dir_iter(p);
                        dir_iter != end_iter; ++dir_iter) {
                    boost::filesystem::remove_all(*dir_iter);
                }
            }
            else {
                boost::filesystem::create_directory(p);
            }

            string dbpathString = p.string();
            dbpath = dbpathString.c_str();

            cmdLine.prealloc = false;

            // dbtest defaults to smallfiles
            cmdLine.smallfiles = true;
            if( params.count("bigfiles") ) {
                cmdLine.dur = true;
            }

            cmdLine.oplogSize = 10 * 1024 * 1024;
            Client::initThread("testsuite");
            acquirePathLock();

            srand( (unsigned) seed );
            printGitVersion();
            printOpenSSLVersion();
            printSysInfo();

            DEV log() << "_DEBUG build" << endl;
            if( sizeof(void*)==4 )
                log() << "32bit" << endl;
            log() << "random seed: " << seed << endl;

            if( time(0) % 3 == 0 && !nodur ) {
                if( !cmdLine.dur ) {
                    cmdLine.dur = true;
                    log() << "****************" << endl;
                    log() << "running with journaling enabled to test that. dbtests will do this occasionally even if --dur is not specified." << endl;
                    log() << "****************" << endl;
                }
            }

            FileAllocator::get()->start();

            vector<string> suites;
            if (params.count("suites")) {
                suites = params["suites"].as< vector<string> >();
            }

            string filter = "";
            if ( params.count( "filter" ) ) {
                filter = params["filter"].as<string>();
            }

            dur::startup();

            if( debug && cmdLine.dur ) {
                log() << "_DEBUG: automatically enabling cmdLine.durOptions=8 (DurParanoid)" << endl;
                // this was commented out.  why too slow or something? : 
                cmdLine.durOptions |= 8;
            }

            TestWatchDog twd;
            twd.go();

            // set tlogLevel to -1 to suppress MONGO_TLOG(0) output in a test program
            tlogLevel = -1;

            int ret = ::mongo::unittest::Suite::run(suites,filter,runsPerTest);

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
