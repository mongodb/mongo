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
#include "../util/version.h"
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>
#include "framework.h"
#include "../util/file_allocator.h"
#include "../db/dur.h"
#include "../util/background.h"

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

namespace po = boost::program_options;

namespace mongo {

    CmdLine cmdLine;

    namespace regression {

        map<string,Suite*> * mongo::regression::Suite::_suites = 0;

        class Result {
        public:
            Result( string name ) : _name( name ) , _rc(0) , _tests(0) , _fails(0) , _asserts(0) {
            }

            string toString() {
                stringstream ss;

                char result[128];
                sprintf(result, "%-20s | tests: %4d | fails: %4d | assert calls: %6d\n", _name.c_str(), _tests, _fails, _asserts);
                ss << result;

                for ( list<string>::iterator i=_messages.begin(); i!=_messages.end(); i++ ) {
                    ss << "\t" << *i << '\n';
                }

                return ss.str();
            }

            int rc() {
                return _rc;
            }

            string _name;

            int _rc;
            int _tests;
            int _fails;
            int _asserts;
            list<string> _messages;

            static Result * cur;
        };

        Result * Result::cur = 0;

        int minutesRunning = 0; // reset to 0 each time a new test starts
        mutex minutesRunningMutex("minutesRunningMutex");
        string currentTestName;

        Result * Suite::run( const string& filter ) {
            // set tlogLevel to -1 to suppress tlog() output in a test program
            tlogLevel = -1;

            log(1) << "\t about to setupTests" << endl;
            setupTests();
            log(1) << "\t done setupTests" << endl;

            Result * r = new Result( _name );
            Result::cur = r;

            /* see note in SavedContext */
            //writelock lk("");

            for ( list<TestCase*>::iterator i=_tests.begin(); i!=_tests.end(); i++ ) {
                TestCase * tc = *i;
                if ( filter.size() && tc->getName().find( filter ) == string::npos ) {
                    log(1) << "\t skipping test: " << tc->getName() << " because doesn't match filter" << endl;
                    continue;
                }

                r->_tests++;

                bool passes = false;

                log(1) << "\t going to run test: " << tc->getName() << endl;

                stringstream err;
                err << tc->getName() << "\t";

                {
                    scoped_lock lk(minutesRunningMutex);
                    minutesRunning = 0;
                    currentTestName = tc->getName();
                }

                try {
                    tc->run();
                    passes = true;
                }
                catch ( MyAssertionException * ae ) {
                    err << ae->ss.str();
                    delete( ae );
                }
                catch ( std::exception& e ) {
                    err << " exception: " << e.what();
                }
                catch ( int x ) {
                    err << " caught int : " << x << endl;
                }
                catch ( ... ) {
                    cerr << "unknown exception in test: " << tc->getName() << endl;
                }

                if ( ! passes ) {
                    string s = err.str();
                    log() << "FAIL: " << s << endl;
                    r->_fails++;
                    r->_messages.push_back( s );
                }
            }

            if ( r->_fails )
                r->_rc = 17;

            log(1) << "\t DONE running tests" << endl;

            return r;
        }

        void show_help_text(const char* name, po::options_description options) {
            cout << "usage: " << name << " [options] [suite]..." << endl
                 << options << "suite: run the specified test suite(s) only" << endl;
        }

        class TestWatchDog : public BackgroundJob {
        public:
            virtual string name() const { return "TestWatchDog"; }
            virtual void run(){

                while (true) {
                    sleepsecs(60);

                    scoped_lock lk(minutesRunningMutex);
                    minutesRunning++; //reset to 0 when new test starts

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

        int Suite::run( int argc , char** argv , string default_dbpath ) {
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
            ("dur", "enable journaling")
            ("nodur", "disable journaling (currently the default)")
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
                for ( map<string,Suite*>::iterator i = _suites->begin() ; i != _suites->end(); i++ )
                    cout << i->first << endl;
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

            int ret = run(suites,filter);

#if !defined(_WIN32) && !defined(__sunos__)
            flock( lockFile, LOCK_UN );
#endif

            cc().shutdown();
            dbexit( (ExitCode)ret ); // so everything shuts down cleanly
            return ret;
        }

        int Suite::run( vector<string> suites , const string& filter ) {
            for ( unsigned int i = 0; i < suites.size(); i++ ) {
                if ( _suites->find( suites[i] ) == _suites->end() ) {
                    cout << "invalid test [" << suites[i] << "], use --list to see valid names" << endl;
                    return -1;
                }
            }

            list<string> torun(suites.begin(), suites.end());

            if ( torun.size() == 0 )
                for ( map<string,Suite*>::iterator i=_suites->begin() ; i!=_suites->end(); i++ )
                    torun.push_back( i->first );

            list<Result*> results;

            for ( list<string>::iterator i=torun.begin(); i!=torun.end(); i++ ) {
                string name = *i;
                Suite * s = (*_suites)[name];
                verify( s );

                log() << "going to run suite: " << name << endl;
                results.push_back( s->run( filter ) );
            }

            Logstream::get().flush();

            cout << "**************************************************" << endl;

            int rc = 0;

            int tests = 0;
            int fails = 0;
            int asserts = 0;

            for ( list<Result*>::iterator i=results.begin(); i!=results.end(); i++ ) {
                Result * r = *i;
                cout << r->toString();
                if ( abs( r->rc() ) > abs( rc ) )
                    rc = r->rc();

                tests += r->_tests;
                fails += r->_fails;
                asserts += r->_asserts;
            }

            Result totals ("TOTALS");
            totals._tests = tests;
            totals._fails = fails;
            totals._asserts = asserts;

            cout << totals.toString(); // includes endl

            return rc;
        }

        void Suite::registerSuite( string name , Suite * s ) {
            if ( ! _suites )
                _suites = new map<string,Suite*>();
            Suite*& m = (*_suites)[name];
            uassert( 10162 ,  "already have suite with that name" , ! m );
            m = s;
        }

        void assert_pass() {
            Result::cur->_asserts++;
        }

        void assert_fail( const char * exp , const char * file , unsigned line ) {
            Result::cur->_asserts++;

            MyAssertionException * e = new MyAssertionException();
            e->ss << "ASSERT FAILED! " << file << ":" << line << endl;
            cout << e->ss.str() << endl;
            throw e;
        }

        void fail( const char * exp , const char * file , unsigned line ) {
            verify(0);
        }

        MyAssertionException * MyAsserts::getBase() {
            MyAssertionException * e = new MyAssertionException();
            e->ss << _file << ":" << _line << " " << _aexp << " != " << _bexp << " ";
            return e;
        }

        void MyAsserts::printLocation() {
            log() << _file << ":" << _line << " " << _aexp << " != " << _bexp << " ";
        }

        void MyAsserts::_gotAssert() {
            Result::cur->_asserts++;
        }

    }

    void setupSignals( bool inFork ) {}

}
