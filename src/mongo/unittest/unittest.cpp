// mongo/unittest/unittest.cpp

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
#include "mongo/unittest/unittest.h"

#include <iostream>
#include <map>

#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    namespace unittest {

        namespace {
            std::map<std::string, Suite *> *_suites = 0;

            mutex currentTestNameMutex("currentTestNameMutex");
            std::string currentTestName;
        }  // namespace

        class Result {
        public:
            Result( const std::string &name ) : _name( name ) , _rc(0) , _tests(0) , _fails(0) , _asserts(0) {}

            std::string toString() {
                std::stringstream ss;

                char result[128];
                sprintf(result, "%-20s | tests: %4d | fails: %4d | assert calls: %6d\n", _name.c_str(), _tests, _fails, _asserts);
                ss << result;

                for ( std::vector<std::string>::iterator i=_messages.begin(); i!=_messages.end(); i++ ) {
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
            std::vector<std::string> _messages;

            static Result * cur;
        };

        Result *Result::cur = 0;

        TestCase::~TestCase() {}
        void TestCase::setUp() {}
        void TestCase::tearDown() {}

        Suite::Suite( const std::string &name ) : _name( name ) {
            registerSuite( name , this );
        }

        Suite::~Suite() {}

        Result * Suite::run( const std::string& filter ) {
            // set tlogLevel to -1 to suppress tlog() output in a test program
            tlogLevel = -1;

            log(1) << "\t about to setupTests" << std::endl;
            setupTests();
            log(1) << "\t done setupTests" << std::endl;

            Result * r = new Result( _name );
            Result::cur = r;

            /* see note in SavedContext */
            //writelock lk("");

            for ( std::vector<TestCase*>::iterator i=_tests.begin(); i!=_tests.end(); i++ ) {
                TestCase * tc = *i;
                if ( filter.size() && tc->getName().find( filter ) == std::string::npos ) {
                    log(1) << "\t skipping test: " << tc->getName() << " because doesn't match filter" << std::endl;
                    continue;
                }

                r->_tests++;

                bool passes = false;

                {
                    scoped_lock lk(currentTestNameMutex);
                    currentTestName = tc->getName();
                }

                log(1) << "\t going to run test: " << tc->getName() << std::endl;

                std::stringstream err;
                err << tc->getName() << "\t";

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
                    err << " caught int : " << x << std::endl;
                }
                catch ( ... ) {
                    err << "unknown exception in test: " << tc->getName() << std::endl;
                }

                if ( ! passes ) {
                    std::string s = err.str();
                    log() << "FAIL: " << s << std::endl;
                    r->_fails++;
                    r->_messages.push_back( s );
                }
            }

            if ( r->_fails )
                r->_rc = 17;


            {
                scoped_lock lk(currentTestNameMutex);
                currentTestName = "";
            }

            log(1) << "\t DONE running tests" << std::endl;

            return r;
        }

        int Suite::run( const std::vector<std::string> &suites , const std::string& filter ) {
            for ( unsigned int i = 0; i < suites.size(); i++ ) {
                if ( _suites->find( suites[i] ) == _suites->end() ) {
                    std::cout << "invalid test suite [" << suites[i] << "], use --list to see valid names" << std::endl;
                    return -1;
                }
            }

            std::vector<std::string> torun(suites);

            if ( torun.empty() ) {
                for ( std::map<std::string,Suite*>::iterator i=_suites->begin() ; i!=_suites->end(); i++ )
                    torun.push_back( i->first );
            }

            std::vector<Result*> results;

            for ( std::vector<std::string>::iterator i=torun.begin(); i!=torun.end(); i++ ) {
                std::string name = *i;
                Suite * s = (*_suites)[name];
                fassert( 16145,  s );

                log() << "going to run suite: " << name << std::endl;
                results.push_back( s->run( filter ) );
            }

            Logstream::get().flush();

            std::cout << "**************************************************" << std::endl;

            int rc = 0;

            int tests = 0;
            int fails = 0;
            int asserts = 0;

            for ( std::vector<Result*>::iterator i=results.begin(); i!=results.end(); i++ ) {
                Result * r = *i;
                std::cout << r->toString();
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

            std::cout << totals.toString(); // includes endl

            return rc;
        }

        void Suite::registerSuite( const std::string &name , Suite * s ) {
            if ( ! _suites )
                _suites = new std::map<std::string,Suite*>();
            Suite*& m = (*_suites)[name];
            fassert( 10162, ! m );
            m = s;
        }

        void assert_pass() {
            Result::cur->_asserts++;
        }

        void assert_fail( const char * exp , const char * file , unsigned line ) {
            Result::cur->_asserts++;

            MyAssertionException * e = new MyAssertionException();
            e->ss << "ASSERT FAILED! " << file << ":" << line << std::endl;
            std::cout << e->ss.str() << std::endl;
            throw e;
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

        std::string getExecutingTestName() {
            scoped_lock lk(currentTestNameMutex);
            return currentTestName;
        }

        std::vector<std::string> getAllSuiteNames() {
            std::vector<std::string> result;
            if (_suites) {
                for ( std::map<std::string, Suite *>::const_iterator i = _suites->begin();
                      i != _suites->end(); ++i ) {

                    result.push_back(i->first);
                }
            }
            return result;
        }

    }  // namespace unittest
}  // namespace mongo
