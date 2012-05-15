
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


    bool thing() {
        return mongo::inConstructorChain();
    }

    bool a;
    bool b;
};

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
            
            {
                MyClass x;
                ASSERT( x.a );
                ASSERT( x.b );
                ASSERT( ! x.thing() );
            }
            
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
