
/**
*    Copyright (C) 2012 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/dbtests/dbtests.h"
#include "mongo/util/stack_introspect.h"

class MyClass {
public:
    MyClass() {
        a = mongo::inConstructorChain();
        b = thing();
    }

    MyClass( int  x ) {
        a = mongo::inConstructorChain();
        b = thing();
    }

    ~MyClass() {
        z = mongo::inConstructorChain();
    }

    bool thing() {
        return mongo::inConstructorChain();
    }

    bool a;
    bool b;

    static bool z;
};

bool MyClass::z;

namespace foo {
    class Bar {
    public:
        Bar() {
            a = mongo::inConstructorChain();
        }

        bool a;
    };
};

namespace StackTests {

    class InCons {
    public:
        void run() {
            for ( int i=0; i<3; i++ ) {
                _run();
            }
        }

        void _run() {
            
            foo::Bar b;
            ASSERT( b.a );
            
            MyClass::z = false;

            {
                MyClass x;
                ASSERT( x.a );
                ASSERT( x.b );
                ASSERT( ! x.thing() );
            }
            
            ASSERT( MyClass::z );

            {
                MyClass x(5);
                ASSERT( x.a );
                ASSERT( x.b );
                ASSERT( ! x.thing() );
            }
            
            ASSERT( ! mongo::inConstructorChain() );
        }
    };
    
    class All : public Suite {
    public:
        
        All() : Suite( "stack" ) {
        }

        void setupTests() {
            if ( inConstructorChainSupported() ) {
#if defined(_DEBUG) && !defined(MONGO_OPTIMIZED_BUILD)
                DEV add< InCons >(); 
#endif
            }
        }
        
    } myall;

}
