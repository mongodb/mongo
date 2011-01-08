#pragma once

#include "../goodies.h" // printStackTrace

namespace mongo {

    /** some self-testing of synchronization and attempts to catch race conditions.

        use something like:

        CodeBlock myBlock;

        void foo() { 
            CodeBlock::Within w(myBlock);
            ...
        }

        In _DEBUG builds, will (sometimes/maybe) fail if two threads are in the same code block at 
        the same time. Also detects and disallows recursion.
    */

#if defined(_DEBUG)

    class CodeBlock { 
        volatile int n;
        unsigned tid;
        void fail() { 
            log() << "synchronization (race condition) failure" << endl;
            printStackTrace();
            abort();
        }
        void enter() { 
            if( ++n != 1 ) fail();
#if defined(_WIN32)
            tid = GetCurrentThreadId();
#endif
        }
        void leave() {
            if( --n != 0 ) fail();
        }
    public:
        CodeBlock() : n(0) { }

        class Within { 
            CodeBlock& _s;
        public:
            Within(CodeBlock& s) : _s(s) { _s.enter(); }
            ~Within() { _s.leave(); }
        };

        void assertWithin() {
            assert( n == 1 );
#if defined(_WIN32)
            assert( GetCurrentThreadId() == tid );
#endif
        }
    };
    
#else

    class CodeBlock{ 
    public:
        class Within { 
        public:
            Within(CodeBlock&) { }
        };
        void assertWithin() { }
    };

#endif

}
