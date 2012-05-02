// mongo/unittest/unittest.h

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

/*

  simple portable regression system
 */

#include <sstream>
#include <string>
#include <vector>

#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

#define ASSERT_THROWS(a,b)                                          \
    try {                                                           \
        a;                                                          \
        mongo::unittest::assert_fail( #a , __FILE__ , __LINE__ );   \
    } catch ( b& ){                                                 \
        mongo::unittest::assert_pass();                             \
    }



#define ASSERT_EQUALS(a,b) mongo::unittest::MyAsserts( #a , #b , __FILE__ , __LINE__ ).ae( (a) , (b) )
#define ASSERT_NOT_EQUALS(a,b) mongo::unittest::MyAsserts( #a , #b , __FILE__ , __LINE__ ).nae( (a) , (b) )

#define ASSERT(x) (void)( (!(!(x))) ? mongo::unittest::assert_pass() : FAIL(x) )
#define FAIL(x) mongo::unittest::assert_fail( #x , __FILE__ , __LINE__ )

namespace mongo {

    namespace unittest {

        class Result;

        class TestCase {
        public:
            virtual ~TestCase();
            virtual void setUp();
            virtual void tearDown();
            virtual void run() = 0;
            virtual std::string getName() = 0;
        };

        class Test : public TestCase {
        public:
            Test();
            virtual ~Test();
            virtual std::string getName();

        protected:
            void setTestName( const std::string &name );

        private:
            std::string _name;
        };

        template< class T >
        class TestHolderBase : public TestCase {
        public:
            TestHolderBase() {}
            virtual ~TestHolderBase() {}
            virtual void run() {
                boost::scoped_ptr<T> t;
                t.reset( create() );
                t->run();
            }
            virtual T * create() = 0;
            virtual std::string getName() {
                return demangleName( typeid(T) );
            }
        };

        template< class T >
        class TestHolder0 : public TestHolderBase<T> {
        public:
            virtual T * create() {
                return new T();
            }
        };

        template< class T , typename A  >
        class TestHolder1 : public TestHolderBase<T> {
        public:
            TestHolder1( const A& a ) : _a(a) {}
            virtual T * create() {
                return new T( _a );
            }
            const A _a;
        };

        class Suite {
        public:
            Suite( const string &name );
            virtual ~Suite();

            template<class T>
            void add() {
                _tests.push_back( new TestHolder0<T>() );
            }

            template<class T , typename A >
            void add( const A& a ) {
                _tests.push_back( new TestHolder1<T,A>(a) );
            }

            Result * run( const std::string& filter );

            static int run( const std::vector<std::string> &suites , const std::string& filter );

        protected:
            virtual void setupTests() = 0;

        private:
            typedef std::vector<TestCase *> TestCaseList;
            std::string _name;
            TestCaseList _tests;
            bool _ran;

            void registerSuite( const std::string &name , Suite *s );
        };

        void assert_pass();
        void assert_fail( const char * exp , const char * file , unsigned line );

        class MyAssertionException : private boost::noncopyable {
        public:
            MyAssertionException() {
                ss << "assertion: ";
            }
            std::stringstream ss;
        };

        class MyAsserts : private boost::noncopyable {
        public:
            MyAsserts( const char * aexp , const char * bexp , const char * file , unsigned line )
                : _aexp( aexp ) , _bexp( bexp ) , _file( file ) , _line( line ) {

            }

            template<typename A,typename B>
            void ae( const A &a , const B &b ) {
                _gotAssert();
                if ( a == b )
                    return;

                printLocation();

                MyAssertionException * e = getBase();
                e->ss << a << " != " << b << std::endl;
                log() << e->ss.str() << std::endl;
                throw e;
            }

            template<typename A,typename B>
            void nae( const A &a , const B &b ) {
                _gotAssert();
                if ( a != b )
                    return;

                printLocation();

                MyAssertionException * e = getBase();
                e->ss << a << " == " << b << std::endl;
                log() << e->ss.str() << std::endl;
                throw e;
            }

            void printLocation();

        private:

            void _gotAssert();

            MyAssertionException * getBase();

            std::string _aexp;
            std::string _bexp;
            std::string _file;
            unsigned _line;
        };

        /**
         * Returns the name of the currently executing unit test
         */
        std::string getExecutingTestName();

        /**
         * Return a list of suite names.
         */
        std::vector<std::string> getAllSuiteNames();

    }  // namespace unittest
}  // namespace mongo
