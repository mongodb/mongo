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

#include <boost/program_options.hpp>

#include "../stdafx.h"
#include "framework.h"
#include "../util/file_allocator.h"

#ifndef _WIN32
#include <cxxabi.h>
#include <sys/file.h>
#endif

namespace po = boost::program_options;

namespace mongo {

    namespace regression {

        map<string,Suite*> * mongo::regression::Suite::_suites = 0;

        class Result {
        public:
            Result( string name ) : _name( name ) , _rc(0) , _tests(0) , _fails(0) , _asserts(0) {
            }

            string toString(){
                stringstream ss;
                ss << _name << " tests:" << _tests << " fails:" << _fails << " assert calls:" << _asserts << "\n";
                for ( list<string>::iterator i=_messages.begin(); i!=_messages.end(); i++ ){
                    ss << "\t" << *i << "\n";
                }
                return ss.str();
            }

            int rc(){
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

        Result * Suite::run(){
            setupTests();

            Result * r = new Result( _name );
            Result::cur = r;

            for ( list<TestCase*>::iterator i=_tests.begin(); i!=_tests.end(); i++ ){
                TestCase * tc = *i;

                r->_tests++;

                bool passes = false;

                log(1) << "\t" << tc->getName() << endl;

                try {
                    tc->run();
                    passes = true;
                }
                catch ( ... ){
                    log() << "unknown exception in test: " << tc->getName() << endl;
                }

                if ( ! passes )
                    r->_fails++;
            }

            return r;
        }

        void show_help_text(const char* name, po::options_description options) {
            cout << "usage: " << name << " [options] [suite]..." << endl
                 << options << "suite: run the specified test suite(s) only" << endl;
        }

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
                ("seed", po::value<unsigned long long>(&seed), "random number seed")
                ;

            hidden_options.add_options()
                ("suites", po::value< vector<string> >(), "test suites to run")
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
            } catch (po::error &e) {
                cout << "ERROR: " << e.what() << endl << endl;
                show_help_text(argv[0], shell_options);
                return EXIT_BADOPTIONS;
            }

            if (params.count("help")) {
                show_help_text(argv[0], shell_options);
                return EXIT_CLEAN;
            }

            if (params.count("debug")) {
                logLevel = 1;
            }

            if (params.count("list")) {
                for ( map<string,Suite*>::iterator i = _suites->begin() ; i != _suites->end(); i++ )
                    cout << i->first << endl;
                return 0;
            }

            boost::filesystem::path p(dbpathSpec);

            /* try removing and recreating the directory
             *
             * we let this fail because some of the test machines have
             * special setups for /tmp/unittest/ which we don't want
             * to destroy. */
            try {
                if (boost::filesystem::exists(p)) {
                    boost::filesystem::remove_all(p);
                }
                boost::filesystem::create_directory(p);
            } catch (std::runtime_error &e) {}

            string dbpathString = p.native_directory_string();
            dbpath = dbpathString.c_str();

            acquirePathLock();

            srand( seed );
            printGitVersion();
            printSysInfo();
            out() << "random seed: " << seed << endl;

            theFileAllocator().start();

            vector<string> suites;
            if (params.count("suites")) {
                suites = params["suites"].as< vector<string> >();
            }
            int ret = run(suites);

#if !defined(_WIN32) && !defined(__sunos__)
            flock( lockFile, LOCK_UN );
#endif

            dbexit( (ExitCode)ret ); // so everything shuts down cleanly
            return ret;
        }

        int Suite::run( vector<string> suites ){
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

            for ( list<string>::iterator i=torun.begin(); i!=torun.end(); i++ ){
                string name = *i;
                Suite * s = (*_suites)[name];
                assert( s );

                log() << "going to run suite: " << name << endl;
                results.push_back( s->run() );
            }

            cout << "**************************************************" << endl;
            cout << "**************************************************" << endl;
            cout << "**************************************************" << endl;

            int rc = 0;

            int tests = 0;
            int fails = 0;
            int asserts = 0;

            for ( list<Result*>::iterator i=results.begin(); i!=results.end(); i++ ){
                Result * r = *i;
                cout << r->toString();
                if ( abs( r->rc() ) > abs( rc ) )
                    rc = r->rc();

                tests += r->_tests;
                fails += r->_fails;
                asserts += r->_asserts;
            }

            cout << "TOTALS  tests:" << tests << " fails: " << fails << " asserts calls: " << asserts << endl;

            return rc;
        }

        void Suite::registerSuite( string name , Suite * s ){
            if ( ! _suites )
                _suites = new map<string,Suite*>();
            Suite*& m = (*_suites)[name];
            uassert( "already have suite with that name" , ! m );
            m = s;
        }

        void assert_pass(){
            Result::cur->_asserts++;
        }

        void assert_fail( const char * exp , const char * file , unsigned line ){
            Result::cur->_asserts++;
            Result::cur->_fails++;

            stringstream ss;
            ss << "ASSERT FAILED! " << file << ":" << line << endl;
            log() << ss.str() << endl;
            Result::cur->_messages.push_back( ss.str() );
        }

        void fail( const char * exp , const char * file , unsigned line ){
            assert(0);
        }

        string demangleName( const type_info& typeinfo ){
#ifdef _WIN32
            return typeinfo.name();
#else
            int status;

            char * niceName = abi::__cxa_demangle(typeinfo.name(), 0, 0, &status);
            if ( ! niceName )
                return typeinfo.name();

            string s = niceName;
            free(niceName);
            return s;
#endif
        }

        void MyAsserts::printLocation(){
            log() << _file << ":" << _line << " " << _aexp << " != " << _bexp << " ";
        }

        void MyAsserts::ae( double a , double b ){
            Result::cur->_asserts++;
            if ( a == b )
                return;

            printLocation();
            log() << a << " != " << b << endl;
            throw -1;
        }

        void MyAsserts::ae( string a , string b ){
            Result::cur->_asserts++;
            if ( a == b )
                return;

            printLocation();
            log() << a << " != " << b << endl;
            throw -1;
        }

    }
}
