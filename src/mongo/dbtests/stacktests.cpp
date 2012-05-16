
#include "pch.h"

#include "dbtests.h"

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
