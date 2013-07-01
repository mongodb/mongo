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

#include "mongo/unittest/unittest.h"

#include <iostream>
#include <map>

#include "mongo/base/init.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/log_manager.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace unittest {

        namespace {
            logger::MessageLogDomain* unittestOutput =
                logger::globalLogManager()->getNamedDomain("unittest");
            typedef std::map<std::string, Suite*> SuiteMap;

            inline SuiteMap& _allSuites() {
                static SuiteMap allSuites;
                return allSuites;
            }

        }  // namespace

        logger::LogstreamBuilder log() {
            return LogstreamBuilder(unittestOutput, getThreadName(), logger::LogSeverity::Log());
        }

        MONGO_INITIALIZER_WITH_PREREQUISITES(UnitTestOutput, ("GlobalLogManager", "default"))(
                InitializerContext*) {

            unittestOutput->attachAppender(
                    logger::MessageLogDomain::AppenderAutoPtr(
                            new logger::ConsoleAppender<logger::MessageLogDomain::Event>(
                                    new logger::MessageEventDetailsEncoder)));
            return Status::OK();
        }

        class Result {
        public:
            Result( const std::string& name ) : _name( name ) , _rc(0) , _tests(0) , _fails(0) , _asserts(0) {}

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

        Result* Result::cur = 0;

        Test::Test() {}
        Test::~Test() {}

        void Test::run() {
            setUp();

            // An uncaught exception does not prevent the tear down from running. But
            // such an event still constitutes an error. To test this behavior we use a
            // special exception here that when thrown does trigger the tear down but is
            // not considered an error.
            try {
                _doTest();
            }
            catch (FixtureExceptionForTesting&) {
                tearDown();
                return;
            }
            catch (TestAssertionFailureException&) {
                tearDown();
                throw;
            }

            tearDown();
        }

        void Test::setUp() {}
        void Test::tearDown() {}

        Suite::Suite( const std::string& name ) : _name( name ) {
            registerSuite( name , this );
        }

        Suite::~Suite() {}

        void Suite::add(const std::string& name, const TestFunction& testFn) {
            _tests.push_back(new TestHolder(name, testFn));
        }

        Result * Suite::run( const std::string& filter, int runsPerTest ) {

            LOG(1) << "\t about to setupTests" << std::endl;
            setupTests();
            LOG(1) << "\t done setupTests" << std::endl;

            Result * r = new Result( _name );
            Result::cur = r;

            for ( std::vector<TestHolder*>::iterator i=_tests.begin(); i!=_tests.end(); i++ ) {
                TestHolder* tc = *i;
                if ( filter.size() && tc->getName().find( filter ) == std::string::npos ) {
                    LOG(1) << "\t skipping test: " << tc->getName() << " because doesn't match filter" << std::endl;
                    continue;
                }

                r->_tests++;

                bool passes = false;

                onCurrentTestNameChange( tc->getName() );

                log() << "\t going to run test: " << tc->getName() << std::endl;

                std::stringstream err;
                err << tc->getName() << "\t";

                try {
                    for ( int x=0; x<runsPerTest; x++ )
                        tc->run();
                    passes = true;
                }
                catch ( const TestAssertionFailureException& ae ) {
                    err << ae.toString();
                }
                catch ( const std::exception& e ) {
                    err << " std::exception: " << e.what() << " in test " << tc->getName();
                }
                catch ( int x ) {
                    err << " caught int " << x << " in test " << tc->getName();
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


            onCurrentTestNameChange( "" );

            log() << "\t DONE running tests" << std::endl;

            return r;
        }

        int Suite::run( const std::vector<std::string>& suites , const std::string& filter , int runsPerTest ) {

            if (_allSuites().empty()) {
                log() << "error: no suites registered.";
                return EXIT_FAILURE;
            }

            for ( unsigned int i = 0; i < suites.size(); i++ ) {
                if ( _allSuites().count( suites[i] ) == 0 ) {
                    log() << "invalid test suite [" << suites[i] << "], use --list to see valid names" << std::endl;
                    return EXIT_FAILURE;
                }
            }

            std::vector<std::string> torun(suites);

            if ( torun.empty() ) {
                for ( SuiteMap::const_iterator i = _allSuites().begin();
                      i !=_allSuites().end(); ++i ) {

                    torun.push_back( i->first );
                }
            }

            std::vector<Result*> results;

            for ( std::vector<std::string>::iterator i=torun.begin(); i!=torun.end(); i++ ) {
                std::string name = *i;
                Suite* s = _allSuites()[name];
                fassert( 16145,  s );

                log() << "going to run suite: " << name << std::endl;
                results.push_back( s->run( filter, runsPerTest ) );
            }

            log() << "**************************************************" << std::endl;

            int rc = 0;

            int tests = 0;
            int fails = 0;
            int asserts = 0;

            for ( std::vector<Result*>::iterator i=results.begin(); i!=results.end(); i++ ) {
                Result* r = *i;
                log() << r->toString();
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

            log() << totals.toString(); // includes endl

            return rc;
        }

        void Suite::registerSuite( const std::string& name , Suite* s ) {
            Suite*& m = _allSuites()[name];
            fassert( 10162, ! m );
            m = s;
        }

        Suite* Suite::getSuite(const std::string& name) {
            Suite* result = _allSuites()[name];
            if (!result)
                result = new Suite(name);  // Suites are self-registering.
            return result;
        }

        void Suite::setupTests() {}

        TestAssertionFailureDetails::TestAssertionFailureDetails(
                const std::string& theFile,
                unsigned theLine,
                const std::string& theMessage )
            : file( theFile ), line( theLine ), message( theMessage ) {
        }

        TestAssertionFailureException::TestAssertionFailureException(
                const std::string& theFile,
                unsigned theLine,
                const std::string& theFailingExpression )
            : _details( new TestAssertionFailureDetails( theFile, theLine, theFailingExpression ) ) {
        }

        std::string TestAssertionFailureException::toString() const {
            std::ostringstream os;
            os << getMessage() << " @" << getFile() << ":" << getLine();
            return os.str();
        }

        TestAssertion::TestAssertion( const char* file, unsigned line )
            : _file( file ), _line( line ) {

            ++Result::cur->_asserts;
        }

        TestAssertion::~TestAssertion() {}

        void TestAssertion::fail( const std::string& message ) const {
            throw TestAssertionFailureException( _file, _line, message );
        }

        ComparisonAssertion::ComparisonAssertion( const char* aexp, const char* bexp,
                                                  const char* file, unsigned line )
            : TestAssertion( file, line ), _aexp( aexp ), _bexp( bexp ) {}

        std::vector<std::string> getAllSuiteNames() {
            std::vector<std::string> result;
            for (SuiteMap::const_iterator i = _allSuites().begin(); i != _allSuites().end(); ++i) {
                    result.push_back(i->first);
            }
            return result;
        }

    }  // namespace unittest
}  // namespace mongo
