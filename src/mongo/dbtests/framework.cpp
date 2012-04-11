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

#include "pch.h"

#include "mongo/dbtests/framework.h"

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>

#include "mongo/db/client.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/dur.h"
#include "mongo/db/instance.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/background.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/version.h"

namespace po = boost::program_options;

namespace mongo {

    CmdLine cmdLine;

    namespace dbtests {

        void show_help_text(const char* name, po::options_description options) {
            cout << "usage: " << name << " [options] [suite]..." << endl
                 << options << "suite: run the specified test suite(s) only" << endl;
        }

        class TestWatchDog : public BackgroundJob {
        public:
            virtual string name() const { return "TestWatchDog"; }
            virtual void run(){

                int minutesRunning = 0;
                std::string lastRunningTestName = ::mongo::unittest::getExecutingTestName();

                while (true) {
                    sleepsecs(60);
                    minutesRunning++;

                    std::string currentTestName = ::mongo::unittest::getExecutingTestName();

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

        int runDbTests( int argc , char** argv , string default_dbpath ) {
            unsigned long long seed = time( 0 );
            string dbpathSpec;

            po::options_description shell_options("options");
            po::options_description hidden_options("Hidden options");
            po::options_description cmdline_options("Command line options");
            po::positional_options_description positional_options;

            shell_options.add_options()
            ("help,h", "show this usage information")
            ("dbpath", po::value<string>(&dbpathSpec)->default_value(default_dbpath),
             "db data path for this test run. NOTE: the contents of this "
             "directory will be overwritten if it already exists")
            ("debug", "run tests with verbose output")
            ("list,l", "list available test suites")
            ("bigfiles", "use big datafiles instead of smallfiles which is the default")
            ("filter,f" , po::value<string>() , "string substring filter on test name" )
            ("verbose,v", "verbose")
            ("dur", "enable journaling (currently the default)")
            ("nodur", "disable journaling")
            ("seed", po::value<unsigned long long>(&seed), "random number seed")
            ("perfHist", po::value<unsigned>(&perfHist), "number of back runs of perf stats to display")
            ;

            hidden_options.add_options()
            ("suites", po::value< vector<string> >(), "test suites to run")
            ("nopreallocj", "disable journal prealloc")
            ;

            positional_options.add("suites", -1);

            cmdline_options.add(shell_options).add(hidden_options);

            po::variables_map params;
            int command_line_style = (((po::command_line_style::unix_style ^
                                        po::command_line_style::allow_guessing) |
                                       po::command_line_style::allow_long_disguise) ^
                                      po::command_line_style::allow_sticky);

            try {
                po::store(po::command_line_parser(argc, argv).options(cmdline_options).
                          positional(positional_options).
                          style(command_line_style).run(), params);
                po::notify(params);
            }
            catch (po::error &e) {
                cout << "ERROR: " << e.what() << endl << endl;
                show_help_text(argv[0], shell_options);
                return EXIT_BADOPTIONS;
            }

            if (params.count("help")) {
                show_help_text(argv[0], shell_options);
                return EXIT_CLEAN;
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
                logLevel = 1;
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
                    cout << "ERROR: path \"" << p.string() << "\" is not a directory" << endl << endl;
                    show_help_text(argv[0], shell_options);
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

            string dbpathString = p.native_directory_string();
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
            printSysInfo();
            DEV log() << "_DEBUG build" << endl;
            if( sizeof(void*)==4 )
                log() << "32bit" << endl;
            log() << "random seed: " << seed << endl;

            if( time(0) % 3 == 0 && !nodur ) {
                cmdLine.dur = true;
                log() << "****************" << endl;
                log() << "running with journaling enabled to test that. dbtests will do this occasionally even if --dur is not specified." << endl;
                log() << "****************" << endl;
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

            int ret = ::mongo::unittest::Suite::run(suites,filter);

#if !defined(_WIN32) && !defined(__sunos__)
            flock( lockFile, LOCK_UN );
#endif

            cc().shutdown();
            dbexit( (ExitCode)ret ); // so everything shuts down cleanly
            return ret;
        }
    }  // namespace dbtests

    void setupSignals( bool inFork ) {}

}  // namespace mongo

