// framework.h

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

#include "../stdafx.h"

#define ASSERT_EXCEPTION(a,b)                                       \
    try {                                                           \
        a;                                                          \
        mongo::regression::assert_fail( #a , __FILE__ , __LINE__ ); \
    } catch ( b& x ){                                               \
        mongo::regression::assert_pass();                           \
    }



#define ASSERT_EQUALS(a,b) (mongo::regression::MyAsserts( #a , #b , __FILE__ , __LINE__ ) ).ae( (a) , (b) )
#define ASSERT(x) (void)( (!(!(x))) ? mongo::regression::assert_pass() : mongo::regression::assert_fail( #x , __FILE__ , __LINE__ ) )
#define FAIL(x) mongo::regression::fail( #x , __FILE__ , __LINE__ )

#include "../db/instance.h"

namespace mongo {

    namespace regression {

        class Result;

        string demangleName( const type_info& typeinfo );

        class TestCase {
        public:
            virtual ~TestCase(){}
            virtual void run() = 0;
            virtual string getName() = 0;
        };

        template< class T >
        class TestHolderBase : public TestCase {
        public:
            TestHolderBase(){}
            virtual ~TestHolderBase(){}
            virtual void run(){
                auto_ptr<T> t;
                t.reset( create() );
                t->run();
            }
            virtual T * create() = 0;
            virtual string getName(){
                return demangleName( typeid(T) );
            }
        };

        template< class T >
        class TestHolder0 : public TestHolderBase<T> {
        public:
            virtual T * create(){
                return new T();
            }
        };

        template< class T , typename A  >
        class TestHolder1 : public TestHolderBase<T> {
        public:
            TestHolder1( const A& a ) : _a(a){}
            virtual T * create(){
                return new T( _a );
            }
            const A& _a;
        };

        class Suite {
        public:
            Suite( string name ) : _name( name ){
                registerSuite( name , this );
                _ran = 0;
            }

            virtual ~Suite() {
                if ( _ran ){
                    DBDirectClient c;
                    c.dropDatabase( "unittests" );
                }
            }

            template<class T>
            void add(){
                _tests.push_back( new TestHolder0<T>() );
            }

            template<class T , typename A >
            void add( const A& a ){
                _tests.push_back( new TestHolder1<T,A>(a) );
            }

            Result * run();

            static int run( vector<string> suites );
            static int run( int argc , char ** argv , string default_dbpath );


        protected:
            virtual void setupTests() = 0;

        private:
            string _name;
            list<TestCase*> _tests;
            bool _ran;

            static map<string,Suite*> * _suites;

            void registerSuite( string name , Suite * s );
        };

        void assert_pass();
        void assert_fail( const char * exp , const char * file , unsigned line );
        void fail( const char * exp , const char * file , unsigned line );

        class MyAsserts {
        public:
            MyAsserts( const char * aexp , const char * bexp , const char * file , unsigned line )
                : _aexp( aexp ) , _bexp( bexp ) , _file( file ) , _line( line ){

            }

            void ae( double a , double b );
            void ae( string a , string b );

            void printLocation();

        private:
            string _aexp;
            string _bexp;
            string _file;
            unsigned _line;
        };

    }
}
