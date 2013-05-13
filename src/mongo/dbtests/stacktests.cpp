
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
                DEV add< InCons >(); 
            }
        }
        
    } myall;

}
